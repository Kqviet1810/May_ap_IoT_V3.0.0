#pragma once
#include <stdint.h>

enum class AutoTunePhase : uint8_t { Idle=0, Preheat=1, Heating=2, Cooling=3, Validating=4, Success=5, Failed=6 };
enum class AutoTuneReason : uint8_t { None=0, SafetyAbort=1, SensorAbort=2, ModeAbort=3, PreheatTimeout=4, PhaseTimeout=5, TotalTimeout=6, NonRepeatable=7, AmplitudeTooSmall=8, PeriodTooSmall=9, InvalidKu=10, InvalidGains=11, SaveFailed=12, Success=13, HeatingTimeout=14, CoolingTimeout=15 };
