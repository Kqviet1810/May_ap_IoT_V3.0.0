#pragma once
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
using std::isfinite;

enum class ControlMode : uint8_t { OnOff, Pid };
enum class AutoTuneState : uint8_t { Idle, Running, Success, Failed };
struct MachineConfig {
  ControlMode controlMode = ControlMode::Pid;
  float kp = 20, ki = 0.04f, kd = 60;
  uint8_t maxHeaterPower = 100, autotuneRelayPowerPercent = 30;
  float tempHysteresis = 0.2f, autotuneBandC = 0.2f;
};
constexpr uint32_t SSR_MIN_ON_MS = 300, SSR_MIN_OFF_MS = 300;
constexpr float THERMAL_PID_BETA = 1.0f;
constexpr float PI = 3.14159265358979323846f;
constexpr float PID_D_FILTER_TAU_SEC = 5.0f;
constexpr float AUTOTUNE_STABILITY_FRACTION = 0.20f;
constexpr uint8_t AUTOTUNE_REQUIRED_CYCLES = 3;
constexpr uint32_t AUTOTUNE_MAX_MS = 2700000;
constexpr uint32_t AUTOTUNE_PHASE_MAX_MS = 900000;
constexpr uint32_t AUTOTUNE_MIN_PERIOD_MS = 10000;
constexpr float AUTOTUNE_MIN_AMPLITUDE_C = 0.10f;
uint32_t elapsedMs(uint32_t now, uint32_t then) { return now - then; }
float clampFloat(float x, float lo, float hi) { return std::max(lo, std::min(hi, x)); }
void sanitizeMachineConfig(MachineConfig &) {}
