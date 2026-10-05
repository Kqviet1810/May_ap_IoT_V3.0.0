  void updateHeatingAndOutputs(uint32_t now) {
    trackAdaptiveEnergy(now);
    if (testModeActive_) { heaterBurst_.reset(); updateTestModeOutputs(now); return; }
    const InputState &in = inputs_.state();
    OutputRequest req{};
    // Cong tac vat ly doi kien tu luc ghi de (hoac chua tung ghi de) - cong
    // tac lay lai quyen dieu khien. Xem ghi chu tai lightWebOverrideActive_.
    if (lightWebOverrideActive_ && in.light != lightWebOverrideRefInput_) {
      lightWebOverrideActive_ = false;
    }
    req.light = lightWebOverrideActive_ ? lightWebOverrideValue_ : in.light; // doc lap AUTO va me

    // Sau mat dien, trong luc dang cho nguoi dung chon TIEP TUC/HUY, tat toan
    // bo co cau, quat hut va nhiet. Den van doc lap de nguoi dung thao tac HMI.
    if (resumeConfirmationRequired_) {
      heaterBurst_.reset();
      req.immediateMasterDrop = true;
      outputs_.update(now, req);
      pid_.reset();
      pidPower_ = 0.0f;
      runtime_.heaterPower = 0.0f;
      return;
    }

    // Canh me vua bat dau (chuyen tu khong chay -> dang chay): dat mot moc
    // tri hoan ngan cho rieng ngo ra quat, de khong dong dien cung luc voi
    // contactor nhiet tong (yeu cau rieng ve dien, xem CIRC_FAN_BATCH_START_
    // STAGGER_MS). Contactor nhiet KHONG bi anh huong - van dong tuc thi.
    if (batchRunning_ && !prevBatchRunningForOutputs_) {
      circFanStaggerUntil_ = now + CIRC_FAN_BATCH_START_STAGGER_MS;
    }
    prevBatchRunningForOutputs_ = batchRunning_;
    const bool circFanStaggerActive = !timeReached(now, circFanStaggerUntil_);

    bool baseCirculation = false;
    const bool protectedBatchControl = batchRunning_;
    if (in.autoMode || protectedBatchControl) {
      // Dang co me: ke ca mat cong tac AUTO, giu PID/quat theo che do bao ve
      // me; AUTO OFF chi khoa dao va sinh E133 sau 120 s. Ngoai me, MANUAL
      // van ton trong quyen dieu khien quat tay nhu cu. circFanStaggerActive
      // chi giu quat cham lai vai tram ms dung luc me vua bat dau - cac
      // dieu kien an toan ben duoi (safetyForcesCirculation) van doc lap va
      // KHONG bi tri hoan.
      baseCirculation = (batchRunning_ && !circFanStaggerActive) || autotune_.running();
    } else {
      baseCirculation = in.circulationFan;
    }
    // Khong cho quat tat cung luc voi bo gia nhiet: giu hau lam mat.
    // CHI xet heaterSsr (dong nhiet THAT su qua D1/PID, van doi hoi batch/
    // autotune dang chay - xem heaterPidConditions ben duoi), KHONG xet
    // heatMaster: tu khi relay tong bam theo cong tac vat ly, no co the BAT
    // ca khi khong co me (cong tac de nguyen) - xet heatMaster o day se lam
    // quat hau lam mat bi kich hoat lai lien tuc, khong bao gio tat duoc.
    if (!baseCirculation && outputs_.state().heaterSsr) {
      postCoolUntil_ = now + POST_COOL_MS;
    }
    const bool postCooling = !timeReached(now, postCoolUntil_);
    const bool sensorStartupGraceActive =
        !timeReached(now, sensorStartupGraceUntil_);
    // Khong bat quat hut chi vi cam bien chua khoi tao xong. Chi lam mat khi
    // loi cam bien da duoc xac nhan va may dang/da co nhiet can xa. (Cung
    // chi xet heaterSsr, ly do nhu tren - khong xet heatMaster.)
    const bool sensorFaultNeedsFan = !sensorUsable_ && !sensorStartupGraceActive &&
        (batchRunning_ || outputs_.state().heaterSsr || postCooling);
    bool scheduledVentActive = false;
    if (!config_.ventAutoEnabled && config_.ventScheduleEnabled &&
        batchRunning_ && rtc_.valid()) {
      const uint8_t hours[VENT_SCHEDULE_MAX_RUNS] = {
        config_.ventScheduleHour1, config_.ventScheduleHour2,
        config_.ventScheduleHour3, config_.ventScheduleHour4,
        config_.ventScheduleHour5, config_.ventScheduleHour6
      };
      const uint8_t count = std::min<uint8_t>(config_.ventScheduleCount,
                                               VENT_SCHEDULE_MAX_RUNS);
      const uint32_t secondOfDay = rtc_.epoch() % 86400UL;
      const uint32_t durationSec =
          static_cast<uint32_t>(config_.ventScheduleDurationMin) * 60UL;
      for (uint8_t i = 0U; i < count; ++i) {
        const uint32_t start = static_cast<uint32_t>(hours[i]) * 3600UL;
        const uint32_t end = start + durationSec;
        const bool inWindow = end <= 86400UL
            ? (secondOfDay >= start && secondOfDay < end)
            : (secondOfDay >= start || secondOfDay < (end - 86400UL));
        if (inWindow) { scheduledVentActive = true; break; }
      }
    }

    bool profileVentActive = false;
    if (config_.ventAutoEnabled && batchRunning_) {
      const uint32_t elapsedSec = elapsedBatchSec(now);
      const uint32_t day = elapsedSec / 86400UL + 1UL;
      const uint8_t duty = ventProfileDutyPercent(config_, day,
          humidityLowActive_);
      const uint32_t cycleSec = static_cast<uint32_t>(config_.ventCycleMinutes) * 60UL;
      const uint32_t onSec = cycleSec * duty / 100UL;
      profileVentActive = (elapsedSec % cycleSec) < onSec;
    }

    const bool safetyForcesCirculation = faults_.circulationForced() ||
        ventTemperatureActive_ || scheduledVentActive || profileVentActive ||
        sensorFaultNeedsFan || postCooling;
    const bool circulation = baseCirculation || safetyForcesCirculation;
    req.circulationFan = circulation;

    if (circulation != previousFanCommand_) {
      previousFanCommand_ = circulation;
      fanOnSince_ = circulation ? now : 0;
    }
    const bool fanStable = circulation && fanOnSince_ != 0U &&
                           elapsedMs(now, fanOnSince_) >= FAN_PRESTART_MS;

    // Thu tu: Emergency/High Temp > CO2 (chua co cam bien, nen khong tao
    // gia tri gia) > profile ngay ap > gioi han RH. RH chi ha duty profile
    // xuong 5%, khong bao gio khoa duong lam mat qua nhiet.
    req.ventFanForceOn = emergencyActive_ || highTemperatureActive_ ||
        ventTemperatureActive_ || faults_.ventForced() || sensorFaultNeedsFan;
    req.ventFan = req.ventFanForceOn || profileVentActive || scheduledVentActive;

    // "Nhiet ngoai me" da bo khoi HMI (khong con cach nao nguoi dung tat lai
    // duoc neu no da tung bat), nen KHONG con duoc phep giu nhiet chay khi
    // khong co me: DUNG ME phai luon luon tat nhiet ngay, khong ngoai le.
    const bool batchAllowsHeat = batchRunning_;
    const bool fanAllowsHeat = !MANUAL_FAN_CAN_DISABLE_HEATING || fanStable;
    // Mat EEPROM giua me: tiep tuc bang cau hinh RAM; cam me moi/nhiet ngoai me.
    const bool storageAllowsHeat = !batchClearPending_ &&
                                   !safetyJournalFaultLatched_ &&
        !storageFaultLatched_ && (!storageDegraded_ || batchRunning_);

    // RELAY NHIET TONG (contactor): cong tac HEATER vat ly la dieu kien cho
    // phep, KHONG phai quyen vuot qua bao ve. Trong moi nhu cau nhiet, bat ky
    // fault co dropHeatMaster=true (gom High/Emergency, sensor STOP...) deu
    // nha contactor tong. Day la lop du phong doc lap neu SSR D1 bi ket/chay
    // ON; ngoai me/AutoTune thi contactor cung khong duoc treo vo ich.
    const bool heatDemandContext = batchRunning_ || autotune_.running();
    // v3.8.0: cong tac vat ly la yeu cau BAT, khong phai quyen vuot qua
    // bao ve. Mat/loi cam bien hoac fault yeu cau cat tong phai nha contactor.
    const bool normalMasterPermit =
        in.heaterEnable && sensorUsable_ && !faults_.masterDropRequired() &&
        !emergencyActive_ && heatDemandContext;
    // D1 SSR/PID (dong nhiet THAT su) van giu NGUYEN VEN toan bo cac dieu
    // kien an toan nhu truoc gio - chi RIENG relay tong o tren la doi theo
    // yeu cau, khong lam long cac dieu kien cho dong dien nhiet thuc te.
    const bool heaterPidConditions = !faults_.masterDropRequired() &&
        storageAllowsHeat && !abnormalResetLatched_ &&
        batchAllowsHeat && sensorUsable_ && fanAllowsHeat &&
        timeReached(now, heatRestartNotBefore_);
    const bool normalSsrPermit = normalMasterPermit && heaterPidConditions &&
        !faults_.ssrInhibited() && !ventTemperatureActive_;
    const bool actuatorReady = outputs_.heaterReady(now) && mayapBootOperationsReady() &&
        !mayapSystemTripLatched() && !mayapFirmwareMaintenanceActive();

    const float effectiveLimit = updateAdaptiveBalance(now, normalSsrPermit && actuatorReady, fanStable, req.ventFan);
    req.ventFan = req.ventFan || adaptiveCoolingRequested();
    float commandedPower = 0.0f;
    if (autotune_.running()) {
      commandedPower = autotune_.power();
    } else if (normalSsrPermit && actuatorReady) {
      if (newSensorSample_) {
        MachineConfig actuatorConfig = config_;
        actuatorConfig.maxHeaterPower = static_cast<uint8_t>(effectiveLimit);
        pidPower_ = pid_.updateOnNewSample(now, config_.targetTemp,
                                           temperature_, actuatorConfig, true);
      }
      commandedPower = std::min(pidPower_, effectiveLimit);
    } else {
      pid_.reset();
      pidPower_ = 0.0f;
    }
    newSensorSample_ = false;

    const bool tunePermit = !faults_.ssrInhibited() &&
                            !faults_.masterDropRequired() &&
                            !storageFaultLatched_ && !storageDegraded_ &&
                            !abnormalResetLatched_ && autotune_.running() &&
                            in.autoMode && in.heaterEnable &&
                            sensorUsable_ && fanStable &&
                            !highTemperatureActive_ && !emergencyActive_;
    const bool masterPermit = normalMasterPermit || tunePermit;
    const bool ssrPermit = (normalSsrPermit || tunePermit) && masterPermit;
    req.heatMaster = masterPermit;
    const auto burst = heaterBurst_.update(now, commandedPower,
        ssrPermit && actuatorReady);
    req.heaterSsr = burst.groupA;
    req.immediateMasterDrop = batchClearPending_ ||
                              safetyJournalFaultLatched_ ||
                              faults_.masterDropRequired() ||
                              emergencyActive_ || !sensorUsable_ ||
                              storageFaultLatched_ ||
                              (storageDegraded_ && !batchRunning_) ||
                              abnormalResetLatched_;

    req.turnLeft = turnPhase_ == TurnPhase::MovingLeft;
    req.turnRight = turnPhase_ == TurnPhase::MovingRight;
    // F-06: coi cung reo cho me qua han chua xac nhan, dung chung co che
    // tam tat (sirenMutedUntil_) voi Nhiet do khan cap - AlarmAck se tu lam
    // moi lai dinh ky dung nhu duoc yeu cau ("keu dinh ky neu bi tat tam").
    // Tu kiem tra coi (sirenSelfTestActive_) la 1 tieng bip NGAN, KHONG lien
    // quan co che tam tat/ACK cua canh bao that (updateSirenSelfTest() da tu
    // bo qua neu dang co canh bao that de khong bao gio trung voi nhanh nay).
    req.siren = ((emergencyActive_ || batchOverdueSirenActive_) &&
                 timeReached(now, sirenMutedUntil_)) || sirenSelfTestActive_;

    // Tao am chi hoat dong trong me, khi cam bien hop le. Hysteresis 2%%RH:
    // dang OFF thi chi bat khi <= SV-2; dang ON thi giu den khi dat SV.
    // Moi nhanh safe/fault cua OutputArbiter van co quyen ep relay OFF ngay.
    const bool humidifierPermit = config_.humidifierInstalled &&
                                  config_.humidifierEnabled && batchRunning_ &&
                                  sensorUsable_ && !faults_.masterDropRequired();
    if (humidifierPermit) {
      if (outputs_.state().humidifier) {
        req.humidifier = humidity_ < config_.targetHumidity;
      } else {
        req.humidifier = humidity_ <=
            (config_.targetHumidity - config_.humidifierHysteresisRh);
      }
    }

    if (mayapFirmwareMaintenanceActive()) {
      heaterBurst_.reset();
      req.heaterSsr = req.heatMaster = req.turnLeft = req.turnRight = false;
      req.immediateMasterDrop = true;
      pid_.reset();
      commandedPower = 0.0f;
    }
    outputs_.update(now, req);
    runtime_.heaterPower = commandedPower;
  }