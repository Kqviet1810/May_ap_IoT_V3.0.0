// AUDIT ARTIFACT ONLY - khong phai production code, khong duoc bien dich vao firmware.
// Sao chep NGUYEN VAN cac ham so hoc thoi gian tu machine_control.h/config.h de
// chay thu tren host (SIMULATED), kiem tra wrap-around millis() va cua so SSR.
#include <cstdio>
#include <cstdint>
#include <algorithm>
#include <cmath>

static uint32_t elapsedMs(uint32_t now, uint32_t then) { return now - then; }
static bool timeReached(uint32_t now, uint32_t target) {
  return static_cast<int32_t>(now - target) >= 0;
}
static float clampFloat(float v, float lo, float hi){ return v<lo?lo:(v>hi?hi:v); }

static const uint32_t SSR_MIN_ON_MS = 300UL, SSR_MIN_OFF_MS = 300UL;
static uint32_t ssrWindowStartedAt_ = 0;
static bool ssrWindowOn(uint32_t now, float power, uint16_t cycleSec) {
  const uint32_t windowMs = std::max<uint32_t>(1000UL, (uint32_t)cycleSec * 1000UL);
  if (ssrWindowStartedAt_ == 0U) ssrWindowStartedAt_ = now;
  const uint32_t elapsed = elapsedMs(now, ssrWindowStartedAt_);
  if (elapsed >= windowMs) ssrWindowStartedAt_ += (elapsed / windowMs) * windowMs;
  const float clamped = clampFloat(power, 0.0f, 100.0f);
  uint32_t onMs = (uint32_t)(clamped * (float)windowMs / 100.0f);
  if (onMs < SSR_MIN_ON_MS) onMs = 0; else if (windowMs - onMs < SSR_MIN_OFF_MS) onMs = windowMs;
  return elapsedMs(now, ssrWindowStartedAt_) < onMs;
}

static int fails = 0;
static void check(const char* name, bool ok){ printf("%-58s %s\n", name, ok?"PASS":"FAIL"); if(!ok) ++fails; }

int main(){
  // 1. elapsedMs / timeReached qua diem tran 2^32
  check("elapsedMs qua wrap (0xFFFFFF00 -> 0x00000100) = 512ms",
        elapsedMs(0x00000100u, 0xFFFFFF00u) == 512u);
  check("timeReached truoc han qua wrap = false",
        timeReached(0xFFFFFF00u, 0x00000100u) == false);
  check("timeReached dung han qua wrap = true",
        timeReached(0x00000100u, 0xFFFFFF00u) == true);
  check("timeReached(now,now) = true", timeReached(12345u,12345u) == true);
  // Gioi han co huu cua so sanh ky hieu: deadline cach > 2^31 ms (~24.85 ngay)
  // bi hieu nguoc. Khong co deadline nao trong firmware nay dat toi nguong do
  // (lon nhat la chu ky dao 720 phut = 43.2e6 ms), nen chi ghi nhan la gioi han.
  check("timeReached voi khoang cach dung 2^31 = false (gioi han da biet)",
        timeReached(0u, 0x80000000u) == false);
  check("Moi deadline thuc te trong firmware < 2^31 ms",
        (uint64_t)720UL*60UL*1000ULL < 0x80000000ULL &&
        (uint64_t)600UL*1000ULL < 0x80000000ULL);

  // 2. Cua so SSR
  ssrWindowStartedAt_ = 0;
  uint32_t t = 1000; uint32_t on = 0;
  for (uint32_t i = 0; i < 10000; ++i, t += 10) if (ssrWindowOn(t, 50.0f, 10)) ++on;
  check("SSR 50% trong 100s cho ~50% thoi gian ON", on > 4800 && on < 5200);
  ssrWindowStartedAt_ = 0; on = 0; t = 1000;
  for (uint32_t i = 0; i < 2000; ++i, t += 10) if (ssrWindowOn(t, 1.0f, 10)) ++on;
  check("SSR 1% (100ms < SSR_MIN_ON_MS) bi ep ve 0", on == 0);
  ssrWindowStartedAt_ = 0; on = 0; t = 1000;
  for (uint32_t i = 0; i < 2000; ++i, t += 10) if (ssrWindowOn(t, 99.0f, 10)) ++on;
  check("SSR 99% (OFF 100ms < SSR_MIN_OFF_MS) bi ep ve 100%", on == 2000);
  ssrWindowStartedAt_ = 0; on = 0; t = 0xFFFFF000u;
  for (uint32_t i = 0; i < 4000; ++i, t += 10) if (ssrWindowOn(t, 50.0f, 10)) ++on;
  check("SSR 50% van dung khi millis() tran giua chu ky", on > 1900 && on < 2100);

  // 3. So hoc lich dao theo epoch
  {
    const uint32_t intervalSec = 120UL*60UL;
    uint32_t lastTurnEpoch = 1700000000UL;
    uint64_t due = (uint64_t)lastTurnEpoch + intervalSec;
    uint32_t nowEpoch = lastTurnEpoch + 100UL;
    uint64_t remainingMs = (due - nowEpoch) * 1000ULL;
    uint32_t nextTurnAt = 5000U + (uint32_t)std::min<uint64_t>(remainingMs, 0x7FFFFFFFULL);
    check("Lich dao: con lai 7100s -> nextTurnAt dung (khong tran)",
          nextTurnAt == 5000U + 7100000U);
    check("Chu ky dao toi da (720 phut) khong cham tran 0x7FFFFFFF",
          (uint64_t)720UL*60UL*1000ULL < 0x7FFFFFFFULL);
  }

  // 4. Bu gio khi phuc hoi - tran uint32
  {
    uint32_t savedElapsed = 0xFFFFFFF0u; uint32_t delta = 100u;
    uint64_t corrected = (uint64_t)savedElapsed + delta;
    uint32_t out = corrected > UINT32_MAX ? UINT32_MAX : (uint32_t)corrected;
    check("Bu gio bi kep tai UINT32_MAX thay vi tran vong", out == UINT32_MAX);
  }

  // 5. Chuoi rang buoc nhiet voi dau vao doc hai
  {
    const float LOW_ALARM_GAP_C=0.3f, HIGH_ALARM_GAP_C=0.3f, HIGH_ALARM_MAX_C=45.0f;
    const float EMERGENCY_ABOVE_HIGH_C=0.3f, EMERGENCY_MAX_C=50.0f;
    const float VENT_ON_ABOVE_SV_C=0.2f, VENT_OFF_ABOVE_SV_C=0.1f, VENT_HYSTERESIS_C=0.2f;
    float target=37.5f, low=99.0f, high=0.0f, emg=0.0f, ventOn=0.0f, ventOff=99.0f;
    low  = clampFloat(low, 25.0f, target-LOW_ALARM_GAP_C);
    high = clampFloat(high, target+HIGH_ALARM_GAP_C, HIGH_ALARM_MAX_C);
    emg  = clampFloat(emg, high+EMERGENCY_ABOVE_HIGH_C, EMERGENCY_MAX_C);
    ventOn = clampFloat(ventOn, target+VENT_ON_ABOVE_SV_C, high);
    ventOff= clampFloat(ventOff, target+VENT_OFF_ABOVE_SV_C, ventOn-VENT_HYSTERESIS_C);
    high = clampFloat(high, ventOn, HIGH_ALARM_MAX_C);
    emg  = clampFloat(emg, high+EMERGENCY_ABOVE_HIGH_C, EMERGENCY_MAX_C);
    check("Sanitize ep dung thu tu LOW<SV<=VENTOFF<VENTON<=HIGH<EMG",
          low<target && target<=ventOff && ventOff<ventOn && ventOn<=high && high<emg);
  }

  printf("\nTONG: %s (%d loi)\n", fails?"CO LOI":"TAT CA PASS", fails);
  return fails?1:0;
}
