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
    pin_start = config.index('constexpr uint8_t MAYAP_USED_PINS[]')
    pin_end = config.index('static_assert(mayapPinsValidAndUnique()',pin_start)
    pin_end = config.index(';',pin_end)+1
    pins = config[pin_start:pin_end]
    pin_names = sorted(set(re.findall(r'\bPIN_\w+\b',pins)))
    declarations=[]
    used=set()
    for name in pin_names:
        declaration = re.search(r'constexpr uint8_t '+name+r'\s*=[^;]*;',config)[0]
        if name=='PIN_OUT_HEATER_SSR_B': declaration='#if defined(MAYAP_HEATER_SSR_B_PIN)\n'+declaration+'\n#endif'
        else: used.add(int(re.search(r'=\s*(\d+)',declaration)[1]))
        declarations.append(declaration)
    pin_source = out / 'pins.cpp'
    pin_source.write_text('#include <cstdint>\n#include <cstddef>\n'+'\n'.join(declarations)+'\n'+pins+'\nint main(){}\n')
    # Verify the actual compile-time guard with hypothetical pin indices only.
    free = next(n for n in range(49) if n not in used)
    a = int(re.search(r'PIN_OUT_HEATER_SSR\s*=\s*(\d+)',config)[1])
    for pin, accepted in [(None,True),(free,True),(a,False),(49,False)]:
        command=['g++','-std=c++14',str(pin_source),'-o',str(out/'pins')]
        if pin is not None: command += ['-DMAYAP_HEATER_SSR_B_PIN='+str(pin)]
        result=subprocess.run(command,capture_output=True,text=True)
        assert (result.returncode==0)==accepted,result.stderr
    print('Actual pinmap: single fallback/optional unique B accepted; collisions/out-of-range rejected (no hardware B pin chosen)')
    start = machine.index('struct OutputRequest {')
    end = body_end(machine, machine.index('class OutputArbiter {', start)) + 1
    source = machine[start:end]
    names = sorted(set(re.findall(r'\b(?:PIN_OUT_\w+|OUTPUT_ACTIVE_HIGH|OUTPUT_EVENT_QUEUE_SIZE|MAX_RELAY_TRANSITIONS_PER_HOUR|HEAT_MASTER_\w+|TURN_DIRECTION_DEADTIME_MS|RELAY_\w+_MS)\b', source)))
    declarations = []
    for name in names:
        found = re.search(r'constexpr [^;\n]*\b' + name + r'\s*=[^;]*;', config)
        if not found: raise ValueError('missing production constant ' + name)
        if name == 'PIN_OUT_HEATER_SSR_B':
            declarations.append('#if defined(MAYAP_HEATER_SSR_B_PIN)\n' + found[0] + '\n#endif')
        else: declarations.append(found[0])
    (out / 'actual-output.inc').write_text('\n'.join(declarations) + '\n' + source)
    start = machine.index('  void updateHeatingAndOutputs(')
    (out / 'actual-heating.inc').write_text(machine[start:body_end(machine,start)])
    heating = machine[start:body_end(machine,start)]
    fields = sorted(set(re.findall(r'config_\.(\w+)', heating)))
    extra = []
    fixture_fields = {'controlMode','kp','ki','kd','maxHeaterPower','autotuneRelayPowerPercent','tempHysteresis','autotuneBandC'}
    for name in fields:
        if name in fixture_fields: continue
        match = re.search(r'  (?:float|bool|uint\d+_t) ' + name + r'\s*=[^;]+;', config)
        if not match: raise ValueError('missing heat configuration ' + name)
        extra.append(match[0])
    (out / 'actual-heating-config.inc').write_text('struct HeatingConfig : MachineConfig {\n' + '\n'.join(extra) + '\n HeatingConfig() { kp=18.0f; ki=0.8f; kd=45.0f; }\n};\n')
    constants = ['CIRC_FAN_BATCH_START_STAGGER_MS','POST_COOL_MS','VENT_SCHEDULE_MAX_RUNS','FAN_PRESTART_MS','MANUAL_FAN_CAN_DISABLE_HEATING','HUMIDIFIER_HYSTERESIS_RH']
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
    # Assert simulator defaults match current production (both OLD and NEW receive these gains).
    for name, expected in [('kp',18.0),('ki',0.8),('kd',45.0)]:
        actual = float(re.search(r'float ' + name + r'\s*=\s*([\d.]+)f;', config)[1])
        if actual != expected: raise ValueError('Update documented simulator defaults: ' + name)
    common = ['g++', '-std=c++11', '-Wall', '-Wextra', '-Werror', '-I', str(out)]
    if args.sanitize: common += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']
    for test in ['thermal-control','thermal-v2','thermal-output','thermal-heating']:
        variants = [1,2] if test in ('thermal-output','thermal-heating') else [0]
        for groups in variants:
            executable = out / (test + str(groups))
            command = common + [str(ROOT / ('tests/' + test + '.cpp')), '-o', str(executable)]
            if groups == 2:
                # Deliberately synthetic HAL index OUTSIDE the ESP32 range: NOT a wiring recommendation.
                command += ['-DMAYAP_HEATER_SSR_B_PIN=49']
            subprocess.run(command, check=True)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            if result.returncode:
                print(result.stdout + result.stderr, flush=True)
                result.check_returncode()
            (args.report_dir / (test + str(groups) + '.log')).write_text(result.stdout)
            print(result.stdout.splitlines()[-1])
    executable = out / 'thermal-plant'
    subprocess.run(common + ['-O2', str(ROOT / 'tests/thermal-plant.cpp'), '-o', str(executable)], check=True)
    with (args.report_dir / 'plant.csv').open('w') as report:
        subprocess.run([str(executable)], stdout=report, check=True)
    print('Thermal model metrics: ' + str(args.report_dir / 'plant.csv'))
    with (args.report_dir / 'plant.csv').open() as report: rows=list(csv.DictReader(report))
    failures=sum(r['targets']=='FAIL' for r in rows)
    print(f'SIMULATION TARGETS: {len(rows)-failures}/{len(rows)} pass; {failures} FAIL. Thresholds unchanged; this is not physical accuracy.')
    if failures and os.getenv('GITHUB_ACTIONS'):
        print(f'::warning::Thermal simulation: {failures}/{len(rows)} miss acceptance targets; review OLD/NEW CSV before commissioning.')
    if failures and args.require_targets: raise SystemExit(1)
