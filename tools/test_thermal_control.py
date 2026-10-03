#!/usr/bin/env python3
"""Test production thermal algorithms and GPIO arbiter; generate honest OLD/NEW metrics."""
import argparse
import csv
import hashlib
import os
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser()
parser.add_argument('--sanitize', action='store_true')
parser.add_argument('--require-targets', action='store_true', help='Fail if any model misses the fixed acceptance targets')
parser.add_argument('--report-dir', type=Path, default=Path('/tmp/mayap-thermal-report'))
args = parser.parse_args()
args.report_dir.mkdir(parents=True, exist_ok=True)
machine = (ROOT / 'MAYAP_INDUSTRIAL_v1_0_0/machine_control.h').read_text()
config = (ROOT / 'MAYAP_INDUSTRIAL_v1_0_0/config.h').read_text()
# Frozen OLD controller from main 9569fcc, not silently replaced by production NEW.
assert hashlib.sha256((ROOT / 'tests/fixtures/thermal-v1/thermal_control.h').read_bytes()).hexdigest() == 'fb2b5828ccdd8997736f9618ae32f3635370c7253f2d1b8efbbd763002d15354'
assert hashlib.sha256((ROOT / 'tests/fixtures/thermal-v1/ssr_window.h').read_bytes()).hexdigest() == '0a7fa13570ec7a29c603f891dd9f38d7bc884503b684e28fb2ff907a0081c1aa'

assert hashlib.sha256((ROOT / 'tests/fixtures/thermal-v1/sensor_filter.h').read_bytes()).hexdigest() == '58d07d62a9d4d345fce56bf39f4c84fb3de7f1072af9636fbcf5838d9753433e'

def body_end(text, start):
    opening = text.index('{', start)
    depth = 0
    for i in range(opening, len(text)):
        if text[i] == '{': depth += 1
        if text[i] == '}':
            depth -= 1
            if depth == 0: return i + 1
    raise ValueError('unclosed production body')

with tempfile.TemporaryDirectory(prefix='mayap-thermal-') as directory:
    out = Path(directory)
    tune_constants = [m[0] for m in re.finditer(r'constexpr [^;\n]*\bAUTOTUNE_\w+\s*=[^;]*;', config)]
    (out / 'actual-autotune-constants.inc').write_text('\n'.join(tune_constants))
    pin_start = config.index('constexpr uint8_t MAYAP_USED_PINS[]')
    pin_end = config.index('static_assert(mayapPinsValidAndUnique()',pin_start)
    pin_end = config.index(';',pin_end)+1
    pins = config[pin_start:pin_end]
    pin_names = sorted(set(re.findall(r'\bPIN_\w+\b',pins)))
    declarations=[]
    used=set()
    for name in pin_names:
        declaration = re.search(r'constexpr uint8_t '+name+r'\s*=[^;]*;',config)[0]
        used.add(int(re.search(r'=\s*(\d+)',declaration)[1]))
        declarations.append(declaration)
    pin_source = out / 'pins.cpp'
    pin_source.write_text('#include <cstdint>\n#include <cstddef>\n'+'\n'.join(declarations)+'\n'+pins+'\nint main(){}\n')
    assert 'constexpr uint8_t HEATER_GROUP_COUNT = 1U;' in config
    assert '#error "This board has one heater control GPIO; both SSRs share GPIO1"' in config
    subprocess.run(['g++','-std=c++14',str(pin_source),'-o',str(out/'pins')],check=True)
    print('Actual pinmap: GPIO1 drives both SSRs as one 16 kW bank; group count fixed at one')
    start = machine.index('struct OutputRequest {')
    end = body_end(machine, machine.index('class OutputArbiter {', start)) + 1
    source = machine[start:end]
    names = sorted(set(re.findall(r'\b(?:PIN_OUT_\w+|OUTPUT_ACTIVE_HIGH|OUTPUT_EVENT_QUEUE_SIZE|MAX_RELAY_TRANSITIONS_PER_HOUR|HEAT_MASTER_\w+|TURN_DIRECTION_DEADTIME_MS|RELAY_\w+_MS)\b', source)))
    declarations = []
    for name in names:
        found = re.search(r'constexpr [^;\n]*\b' + name + r'\s*=[^;]*;', config)
        if not found: raise ValueError('missing production constant ' + name)
        declarations.append(found[0])
    (out / 'actual-output.inc').write_text('\n'.join(declarations) + '\n' + source)
    start = machine.index('  void updateHeatingAndOutputs(')
    (out / 'actual-heating.inc').write_text(machine[start:body_end(machine,start)])
    heating = machine[start:body_end(machine,start)]
    evidence_start = machine.index('    const bool responseDemand = batchRunning_')
    evidence_end = machine.index('    const bool sensorGrace', evidence_start)
    (out / 'actual-heater-evidence.inc').write_text(machine[evidence_start:evidence_end])
    initializer = re.search(r'HeaterBurstScheduler heaterBurst_\{[^;]+;', machine)[0]
    assert initializer == 'HeaterBurstScheduler heaterBurst_{HEATER_GROUP_COUNT, HEATER_BURST_QUANTUM_MS};'
    (out / 'actual-burst-member.inc').write_text(initializer)
    fields = sorted(set(re.findall(r'config_\.(\w+)', heating)) | {'pidCycleSec'})
    extra = []
    fixture_fields = {'controlMode','kp','ki','kd','maxHeaterPower','autotuneRelayPowerPercent','tempHysteresis','autotuneBandC'}
    for name in fields:
        if name in fixture_fields: continue
        match = re.search(r'  (?:float|bool|uint\d+_t) ' + name + r'\s*=[^;]+;', config)
        if not match: raise ValueError('missing heat configuration ' + name)
        extra.append(match[0])
    (out / 'actual-heating-config.inc').write_text('struct HeatingConfig : MachineConfig {\n' + '\n'.join(extra) + '\n HeatingConfig() { kp=18.0f; ki=0.8f; kd=45.0f; }\n};\n')
    assert re.search(r'uint16_t heaterStuckDurationSec\s*=\s*900;', config), 'E115 test must match production default'
    constants = ['CIRC_FAN_BATCH_START_STAGGER_MS','POST_COOL_MS','VENT_SCHEDULE_MAX_RUNS','FAN_PRESTART_MS','MANUAL_FAN_CAN_DISABLE_HEATING','HUMIDIFIER_HYSTERESIS_RH','HEATER_BURST_QUANTUM_MS']
    (out / 'actual-heating-constants.inc').write_text('\n'.join(re.search(r'constexpr [^;\n]*\b'+n+r'\s*=[^;]*;',config)[0] for n in constants))
    start = machine.index('  void updateFilter(')
    end = machine.index('  void completeCycleSuccess(', start)
    iir = '\n'.join(re.search(r'constexpr [^;]*\b' + name + r'\s*=[^;]*;', machine)[0]
                    for name in ['IIR_NUMERATOR','IIR_DENOMINATOR'])
    # The simulation uses the production median/IIR methods, not a redesigned filter.
    (out / 'actual-filter.inc').write_text('namespace SHT485Config {\n' + iir + '\n}\n'
        + 'class ActualSensorFilter { public:\n float value() const { return filteredTemp_; }\n'
        + machine[start:end]
        + ' private: float tempWindow_[3]{}, humWindow_[3]{}; uint8_t windowIndex_=0,windowCount_=0;\n'
        + 'float filteredTemp_=0, filteredHum_=0; bool filterInitialized_=false;\n};\n')
    # Compile the real config serializers, CRC validators and EEPROM migration
    # path against a byte-backed EEPROM. No alternate migration implementation.
    def method(signature):
        start = machine.index(signature)
        return machine[start:body_end(machine, start)]
    cfg_start = config.index('struct MachineConfig {')
    cfg_struct = config[cfg_start:body_end(config, cfg_start)+1]
    sanitize = method('inline void sanitizeMachineConfig(')
    cfg_names = sorted(set(re.findall(r'\b[A-Z][A-Z0-9_]+\b', cfg_struct + sanitize)))
    cfg_constants = []
    for name in cfg_names + ['EEPROM_ADDR_CONFIG_A','EEPROM_ADDR_CONFIG_B','HEATER_BURST_QUANTUM_MS']:
        found = re.search(r'constexpr [^;\n]*\b'+name+r'\s*=[^;]*;', config)
        if found and found[0] not in cfg_constants: cfg_constants.append(found[0])
    enums = '\n'.join(re.search(r'enum class '+name+r'[^;]+;', config)[0]
                      for name in ['ControlMode','TurnDirection','ConnectivityMode'])
    records = machine[machine.index('struct PackedMachineConfigV1 {'):machine.index('struct PackedBatchV1 {')]
    schemas = '\n'.join(re.findall(r'constexpr [^;\n]*\bCONFIG_(?:MAGIC|SCHEMA\w*)\s*=[^;]*;', machine))
    validators = machine[machine.index('  static bool validConfig('):machine.index('  static bool validBatch(')]
    (out / 'actual-config.inc').write_text('\n'.join(cfg_constants)+'\n'+enums+'\n'+cfg_struct+'\n'
        +sanitize+'\n#pragma pack(push,1)\n'+records+'\n#pragma pack(pop)\n'+schemas+'\n'
        +method('inline uint32_t mcCrc32(')+'\n'+method('inline PackedMachineConfigV1 packConfig(')+'\n'
        +method('inline MachineConfig unpackConfig(')+'\n'+method('inline uint8_t ventProfileDutyPercent(')+'\n')
    (out / 'actual-config-load.inc').write_text(method('  bool loadConfig(')+'\n'+method('  bool saveConfig(')+'\n'
        +method('  static bool newer(')+'\n'+validators+'\n'+method('  bool refreshConfigCache('))
    extra_constants = ['THERMAL_PID_BETA','PID_D_FILTER_TAU_SEC','HEAT_RESTART_LOCKOUT_MS','POST_COOL_MS',
        'EVENT_LOG_RAM_SIZE','HMI_EVENT_DISPLAY_CAPACITY','CIRC_FAN_BATCH_START_STAGGER_MS','FAN_PRESTART_MS',
        'MANUAL_FAN_CAN_DISABLE_HEATING']
    (out / 'actual-tune-extra.inc').write_text('\n'.join(re.search(r'constexpr [^;\n]*\b'+n+r'\s*=[^;]*;', config)[0] for n in extra_constants))
    event_types = ''
    for name in ['HmiEventItem','HmiEventSnapshot']:
        start=config.index('struct '+name+' {');event_types+=config[start:body_end(config,start)+1]+'\n'
    start=machine.index('enum class EventType :')
    end=body_end(machine,machine.index('class EventLog {',start))+1
    (out / 'actual-event.inc').write_text(event_types+machine[start:end])
    for name, signature in [('start','  bool startAutoTune('),('update','  void updateAutoTune(')]:
        (out / ('actual-tune-'+name+'.inc')).write_text(method(signature))
    (out / 'actual-safety-thresholds.inc').write_text('\n'.join(
        'constexpr double MODEL_'+name.upper()+' = '+re.search(r'float '+name+r'\s*=\s*([\d.]+)f;',config)[1]+';'
        for name in ['highTempAlarm','emergencyTemp']))
    # Assert simulator defaults match current production (both OLD and NEW receive these gains).
    for name, expected in [('kp',18.0),('ki',0.8),('kd',45.0)]:
        actual = float(re.search(r'float ' + name + r'\s*=\s*([\d.]+)f;', config)[1])
        if actual != expected: raise ValueError('Update documented simulator defaults: ' + name)
    common = ['g++', '-std=c++11', '-Wall', '-Wextra', '-Werror', '-I', str(out)]
    if args.sanitize: common += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']
    plant_source=(ROOT / 'tests/thermal-plant.cpp').read_text()
    (out / 'actual-plants.inc').write_text(re.search(r'const Plant plants\[\]=[^;]+;', plant_source)[0])
    for test in ['thermal-autotune','thermal-control','thermal-v2','thermal-output','thermal-heating','thermal-e115','thermal-config','thermal-filter']:
        variants = [1] if test in ('thermal-output','thermal-heating') else [0]
        for groups in variants:
            executable = out / (test + str(groups))
            command = common + [str(ROOT / ('tests/' + test + '.cpp')), '-o', str(executable)]
            subprocess.run(command, check=True)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            if result.returncode:
                print(result.stdout + result.stderr, flush=True)
                result.check_returncode()
            (args.report_dir / (test + str(groups) + '.log')).write_text(result.stdout)
            print(result.stdout.splitlines()[-1])
            if test == 'thermal-filter':
                (args.report_dir / 'filter-compatibility.csv').write_text(
                    'samples,max_abs_delta_c,mean_abs_delta_c,rms_delta_c\n'
                    +next(line[7:] for line in result.stdout.splitlines() if line.startswith('FILTER,'))+'\n')
            if test == 'thermal-v2':
                with (args.report_dir / 'low-duty.csv').open('w') as bank_report:
                    bank_report.write('quantum_ms,power_percent,horizon_s,requested_pct,delivered_pct,absolute_energy_error_j,max_no_heat_ms,transitions_per_hour\n')
                    bank_report.writelines(line[5:]+'\n' for line in result.stdout.splitlines() if line.startswith('BANK,'))
    executable = out / 'thermal-autotune-plant'
    subprocess.run(common + ['-O2', str(ROOT / 'tests/thermal-autotune-plant.cpp'), '-o', str(executable)], check=True)
    with (args.report_dir / 'autotune-plant.csv').open('w') as report:
        subprocess.run([str(executable), str(args.report_dir / 'autotune-cycles.csv')], stdout=report, check=True)
    tune_rows=list(csv.DictReader((args.report_dir / 'autotune-plant.csv').open()))
    assert len(tune_rows)==864
    for preheat in [30,40,50]:
        subset=[r for r in tune_rows if float(r['preheat_power'])==preheat and r['start_condition']=='COLD']
        print(f'AUTOTUNE preheat {preheat}%: '+str(sum(r['success']=='1' for r in subset))+'/216 SUCCESS; failures bounded and gains retained')
    for relay in [20,30,40]:
        subset=[r for r in tune_rows if float(r['preheat_power'])==30 and float(r['relay_power'])==relay and r['start_condition']=='COLD']
        print(f'AUTOTUNE candidate preheat30/relay{relay}: '+str(sum(r['success']=='1' for r in subset))+'/72 SUCCESS')
    executable = out / 'thermal-plant'
    subprocess.run(common + ['-O2', str(ROOT / 'tests/thermal-plant.cpp'), '-o', str(executable)], check=True)
    with (args.report_dir / 'plant.csv').open('w') as report:
        subprocess.run([str(executable)], stdout=report, check=True)
    print('Thermal model metrics: ' + str(args.report_dir / 'plant.csv'))
    with (args.report_dir / 'plant.csv').open() as report: rows=list(csv.DictReader(report))
    qualification = ['plant','scenario','setpoint','ambient','dead_s','resolution','algorithm','quantum_ms',
        'first_high_cross_s','first_emergency_cross_s','peak_before_high','peak_before_emergency',
        'high_crossed','emergency_crossed','qualification','model_scope']
    with (args.report_dir / 'safety-qualification.csv').open('w') as report:
        writer=csv.DictWriter(report, fieldnames=qualification, lineterminator='\n'); writer.writeheader()
        writer.writerows({k:r[k] for k in qualification} for r in rows)
    sp375=[r for r in rows if r['algorithm']=='NEW' and r['quantum_ms']=='300' and r['setpoint']=='37.5']
    print(f'CONTROL-ONLY / NO PRODUCTION SAFETY INTERVENTION: NEW300 SP37.5 High '
          f'{sum(r["high_crossed"]=="1" for r in sp375)}/{len(sp375)}, Emergency '
          f'{sum(r["emergency_crossed"]=="1" for r in sp375)}/{len(sp375)}')
    failures=sum(r['targets']=='FAIL' for r in rows)
    print(f'SIMULATION TARGETS: {len(rows)-failures}/{len(rows)} pass; {failures} FAIL. Thresholds unchanged; this is not physical accuracy.')
    # Calibrated performance is experimental, but the observed anti-windup
    # sticking case is a deterministic software regression and must fail CI.
    stuck_case=[r for r in rows if r['plant']=='light' and r['scenario']=='cold_start'
        and r['setpoint']=='30.0' and r['ambient']=='28.0' and r['dead_s']=='30'
        and r['resolution']=='0.01' and r['algorithm']=='NEW' and r['quantum_ms']=='300']
    assert len(stuck_case)==1
    stuck=stuck_case[0]
    assert float(stuck['mean_abs_error'])<0.5 and abs(float(stuck['bias']))<0.5 \
        and float(stuck['requested_tail_pct'])<3.0, 'Anti-windup stuck case regressed'
    print('HARD REGRESSION: SP30/ambient28/dead30/res0.01 no positive-heater tail while hot PASS')
    if failures and os.getenv('GITHUB_ACTIONS'):
        print(f'::warning::Thermal simulation: {failures}/{len(rows)} miss acceptance targets; review OLD/NEW CSV before commissioning.')
    if failures and args.require_targets: raise SystemExit(1)
