#pragma once

// Applied at the final arbiter as well as the controller startup branch.
// Normal operation cannot bypass it through a test/manual/remote command.
// Only safety cooling and emergency siren may run behind the splash screen.
template <typename Request>
inline void mayapApplyStartupOutputPolicy(Request &request, bool ready,
                                         bool cooling, bool emergencySiren) {
  if (ready) return;
  request.heaterSsr = request.heatMaster = false;
  request.turnLeft = request.turnRight = false;
  request.light = request.humidifier = false;
  request.immediateMasterDrop = true;
  request.circulationFan = request.ventFan = cooling;
  request.ventFanForceOn = request.ventFanBypassTiming = cooling;
  request.siren = emergencySiren;
}
