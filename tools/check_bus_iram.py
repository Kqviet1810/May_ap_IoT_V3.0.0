"""Verify cache-safe GPIO handlers in the actual linked ESP32-S3 binary."""
import argparse
import re
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--objdump', required=True)
parser.add_argument('--elf', required=True)
args = parser.parse_args()
symbols = subprocess.check_output([args.objdump, '-t', '-C', args.elf], text=True)
entries = []
for line in symbols.splitlines():
    match = re.match(r'^([0-9a-f]+)\s+.*?\s(\.[\w.]+)\s+[0-9a-f]+\s+(.+)$', line)
    if match:
        entries.append((int(match[1], 16), match[2], match[3]))
functions = ('MayapAttinyBusInternal::busIsr(void*)', 'rotaryEncoderIsr(void*)',
             'esp_timer_get_time',
             'xPortEnterCriticalTimeout', 'vPortExitCritical')
for prefix in functions:
    matches = [entry for entry in entries if entry[2].startswith(prefix)]
    assert matches and all('.iram' in entry[1] for entry in matches), (prefix, matches)
for name in ('QUADRATURE_TABLE', 'MayapAttinyBusInternal::edgeAtUs_',
             'MayapAttinyBusInternal::rxMux_'):
    matches = [entry for entry in entries if entry[2] == name]
    assert matches and all('.dram' in entry[1] for entry in matches), (name, matches)
disassembly = subprocess.check_output([args.objdump, '-d', '-C', args.elf], text=True)
handlers = []
for name in ('MayapAttinyBusInternal::busIsr(void*)', 'rotaryEncoderIsr(void*)'):
    match = re.search(r'^[0-9a-f]+ <' + re.escape(name) + r'>:\n(.*?)(?=\n[0-9a-f]+ <|\Z)',
                      disassembly, re.M | re.S)
    assert match, name
    handlers.append(match[1])
for handler in handlers:
    for target in re.findall(r'\bcall\w*\s+([0-9a-f]+)\s+<', handler):
        matches = [entry for entry in entries if entry[0] == int(target, 16)]
        assert matches and all('.flash' not in entry[1] for entry in matches), (target, matches)
print('Linked binary: Tiny/encoder GPIO handlers, callees and shared data are cache-safe IRAM/DRAM')
