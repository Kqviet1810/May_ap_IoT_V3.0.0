"""Exercise production turning methods with a deterministic RTC/input HAL."""
import pathlib, re, subprocess, tempfile, argparse
root = pathlib.Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('--check-regression', action='store_true')
args = p.parse_args()
src = (root / 'MAYAP_INDUSTRIAL_v1_0_0/machine_control.h').read_text()
turn = src[src.index('  bool turningLockdownActive('):src.index('  void latchTurnFault(')]
mode = src[src.index('  void processInputModeTransition('):src.index('  // ----------------------------- Turning')]
with tempfile.TemporaryDirectory() as d:
    d = pathlib.Path(d)
    (d / 'actual-turning.inc').write_text(mode + turn)
    enums = ''
    for name in ('EventCode', 'EventType', 'FaultCode'):
        values = sorted(set(re.findall(name + r'::(\w+)', mode + turn)))
        enums += 'enum class ' + name + ' { ' + ','.join(values) + ' };\n'
    (d / 'actual-turn-enums.inc').write_text(enums)
    command = ['g++', '-std=c++11', '-Wall', '-Wextra', '-Werror', '-I', str(d), str(root / 'tests/runtime-turning.cpp'), '-o', str(d / 'test')]
    subprocess.run(command, check=True)
    subprocess.run([str(d / 'test')], check=True)
    if args.check_regression:
        # Reintroduce the exact historical homing bug only in the temporary HAL.
        old = turn.replace('    scheduleNextTurnFromAnchor(now);\n    mayapSerialPrintf(false, "[TURN] HOME', '    setTurnScheduleAnchor(now);\n    mayapSerialPrintf(false, "[TURN] HOME')
        assert old != turn
        (d / 'actual-turning.inc').write_text(mode + old)
        subprocess.run(command, check=True)
        assert subprocess.run([str(d / 'test')], capture_output=True).returncode != 0
        print('Historical homing re-anchor regression detected')
