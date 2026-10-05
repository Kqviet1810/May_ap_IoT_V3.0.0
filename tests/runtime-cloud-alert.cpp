// Actual mailbox, queue, fault formatting, priority dispatch and receipt parser.
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <ArduinoJson.h>
#define portMUX_INITIALIZER_UNLOCKED 0
using portMUX_TYPE=int;
#define portENTER_CRITICAL(x) ((void)x)
#define portEXIT_CRITICAL(x) ((void)x)
uint32_t clockMs=1000;
uint32_t millis(){return clockMs;}
void mayapSerialPrintf(bool,const char *,...){}
#include "actual-cloud_fault_events.inc"
#include "actual-cloud_alarm_receipt.inc"
constexpr uint8_t CLOUD_OUTBOX_SIZE=16,CLOUD_ACTIVE_TRACK_SIZE=48;
constexpr uint32_t CLOUD_REPEAT_CRITICAL_EMERGENCY_MS=120000,CLOUD_REPEAT_CRITICAL_STOP_MS=300000,
 CLOUD_REPEAT_WARNING_MS=600000,CLOUD_REPEAT_INFO_MS=1800000,CLOUD_MIN_SEND_GAP_MS=3000;
struct BackoffTimer {uint32_t next=0;bool ready(uint32_t t)const{return int32_t(t-next)>=0;}
 void reset(uint32_t t){next=t;}void onSuccess(){next=0;}void onFailure(uint32_t t){next=t+1000;}};
namespace MayapCloudInternal {
static bool knownRuntimeValid=true;
enum class NotifyLevel:uint8_t {Info,Warning,Critical,System};
uint32_t elapsedMs(uint32_t t,uint32_t p){return t-p;}
bool timeReached(uint32_t t,uint32_t p){return int32_t(t-p)>=0;}
struct Runtime{float temperature=37.5f,humidity=60;bool batchRunning=false,powerLossRecovery=false,resumeConfirmationRequired=false;uint8_t currentDay=1;} processingRuntime;
struct Config{uint8_t totalIncubationDays=21;} processingConfig;
#include "actual-cloud-outbox.inc"
#include "actual-cloud-enqueue.inc"
#include "actual-cloud-oneshots.inc"
#include "actual-cloud-faults.inc"
enum class ConnectivityMode{Offline,Online};
struct NetworkStatus {ConnectivityMode requestedMode=ConnectivityMode::Online;bool connected=true;} net;
NetworkStatus mayapGetRawNetworkStatus(){return net;}
NetworkStatus mayapGetNetworkStatus(){return net;}
std::vector<std::string> sent;
bool deferred=false,success=true;
bool sendAlarm(OutboxItem &item){requestDeferred=deferred;if(!deferred){item.attempted=true;sent.push_back(item.alarmType);}return !deferred&&success;}
#include "actual-cloud-drain.inc"
}
int main(){
 using namespace MayapCloudInternal;
 assert(lastRequestFinishedAt==0);
 assert(!mayapDurableAlarmReceipt("{\"success\":true,\"notification_sent\":0}","id"));
 assert(!mayapDurableAlarmReceipt("{\"success\":true,\"durable\":true,\"event_id\":\"other\"}","id"));
 assert(!mayapDurableAlarmReceipt("bad-json","id"));
 assert(mayapDurableAlarmReceipt("{\"success\":true,\"durable\":true,\"event_id\":\"id\"}","id"));
 // A full routine queue must not mark a batch/power notification as sent.
 for(unsigned n=0;n<12;++n){char id[24];snprintf(id,sizeof(id),"INFO_%u",n);assert(enqueueRaw(id,NotifyLevel::Info,false,"info",false,0,0));}
 checkTransitions(clockMs);processingRuntime.batchRunning=true;checkTransitions(clockMs);
 processingRuntime.powerLossRecovery=true;checkPowerRestored(clockMs);
 assert(!lastBatchRunning&&!powerRestoreReported);
 outboxHead=outboxTail=outboxCount=0;
 checkTransitions(clockMs);checkPowerRestored(clockMs);
 assert(lastBatchRunning&&powerRestoreReported&&outboxCount==2);
 outboxHead=outboxTail=outboxCount=0;
 // A 55ms fault survives a long HTTPS block; no reliance on HMI or scan period.
 mayapCloudRecordFault(101,2,true,1000);mayapCloudRecordFault(101,2,false,1055);
 checkFaults(9000);assert(outboxCount==2&&!outbox[0].resolved&&outbox[1].resolved);
 assert(outbox[0].detectedAt==1000&&outbox[1].detectedAt==1055);
 deferred=true;drainOutbox(clockMs);assert(outboxCount==2&&lastSendAt==0);
 deferred=false;drainOutbox(clockMs);assert(outboxCount==1);
 drainOutbox(clockMs);assert(outboxCount==0); // critical bypasses routine 3s gap
 // More than twelve simultaneous faults are all visible to Cloud.
 for(uint16_t c=200;c<220;++c)mayapCloudRecordFault(c,2,true,clockMs);
 checkFaults(clockMs);assert(outboxCount==16&&MayapCloudFaultEvents::count==4);
 assert(outboxCriticalDropped==0);
 while(outboxCount || MayapCloudFaultEvents::count){drainOutbox(clockMs);checkFaults(clockMs);}
 assert(sent.size()==22);
 // Routine messages do not delay critical; same-alarm ordering remains intact.
 enqueueRaw("INFO",NotifyLevel::Info,false,"info",false,0,0);
 enqueueRaw("FAULT_130",NotifyLevel::Critical,false,"off",false,0,0);
 enqueueRaw("FAULT_130",NotifyLevel::Critical,true,"on",false,0,0);
 drainOutbox(clockMs);assert(sent.back()=="FAULT_130"&&outboxCount==2);
 assert(outbox[1].resolved);drainOutbox(clockMs);assert(outboxCount==1);
 // Per-event retry: a failed event cannot stall a fresh critical alarm.
 outboxHead=outboxTail=outboxCount=0;
 enqueueRaw("OLD_CRITICAL",NotifyLevel::Critical,false,"old",false,0,0);
 success=false;drainOutbox(clockMs);
 enqueueRaw("NEW_CRITICAL",NotifyLevel::Critical,false,"new",false,0,0);
 success=true;drainOutbox(clockMs);assert(sent.back()=="NEW_CRITICAL");
 assert(outboxCount==1&&std::string(outbox[outboxHead].alarmType)=="OLD_CRITICAL");
 outboxHead=outboxTail=outboxCount=0;
 enqueueRaw("INFO",NotifyLevel::Info,false,"info",false,0,0);
 // Critical reserve and mailbox priority prevent a warning flood from
 // hiding FAULT_130, including when it sits behind earlier warning edges.
 outboxHead=outboxTail=outboxCount=0;
 for(unsigned n=0;n<12;++n){char id[24];snprintf(id,sizeof(id),"WARN_%u",n);assert(enqueueRaw(id,NotifyLevel::Warning,false,"warn",false,0,0));}
 assert(!enqueueRaw("WARN_FULL",NotifyLevel::Warning,false,"warn",false,0,0));
 mayapCloudRecordFault(401,1,true,clockMs);
 mayapCloudRecordFault(130,2,true,clockMs);
 checkFaults(clockMs);assert(outboxCount==13);
 drainOutbox(clockMs);assert(sent.back()=="FAULT_130");
 outboxHead=outboxTail=outboxCount=0;checkFaults(clockMs);
 outboxHead=outboxTail=outboxCount=0;
 enqueueRaw("INFO",NotifyLevel::Info,false,"info",false,0,0);
 // Once submitted, a payload cannot mutate underneath a durable event ID.
 auto &pending=outbox[outboxHead];pending.attempted=true;
 enqueueRaw("INFO",NotifyLevel::Info,false,"changed",false,0,0);
 assert(outboxCount==2&&std::string(outbox[outboxHead].message)=="info");
 // Mailbox overflow is counted and converges to true current state.
 using namespace MayapCloudFaultEvents;
 head=count=0;for(auto &s:states)s=State{};
 for(unsigned n=0;n<80;++n) record(130,2,(n%2)==0,n);
 assert(count==CAPACITY&&overflow==16);
 Event edge;while(peek(edge))consume(edge);
 assert(!states[0].event.active&&!states[0].resync);
 // millis wrap is supported by the real dispatch arithmetic.
 assert(timeReached(20,0xfffffff0U));
 std::puts("Cloud alert regression: transient fault, >12 faults, backpressure, priority, immutable IDs, durable ACK, overflow resync and wrap PASS");
}
