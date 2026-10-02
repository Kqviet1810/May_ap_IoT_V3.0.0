"""Check the supplied LINKFIX source against the ESP v4 wire contract."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
tiny = (root / 'ATTINY13A_POWER_ALARM/ATTINY13A_POWER_ALARM.ino').read_text(encoding='utf-8')
cfg = (root / 'MAYAP_INDUSTRIAL_v4_0_0/config.h').read_text(encoding='utf-8')
bus = (root / 'MAYAP_INDUSTRIAL_v4_0_0/attiny_bus.h').read_text(encoding='utf-8')

assert 'PROTOCOL_VERSION = 4U' in tiny and 'ATTINY_PROTOCOL_VERSION = 4U' in cfg
table = re.search(r'ATTINY_COMMAND_WIDTH_MS\[8\]\s*=\s*\{([^}]+)\}', cfg)[1]
widths = [int(v) for v in re.findall(r'(\d+)U', table)]
windows = [(int(lo), int(hi), int(cmd)) for lo, hi, cmd in re.findall(
    r'if \(w >= (\d+)\s*&& w <= (\d+)\)\s*return (\d+);', tiny)]
assert len(windows) == 7 and len(widths) == 8
limits = {}
for name in ('minimumMs', 'maximumMs'):
    values = re.search(name + r'\[\]\s*=\s*\{([^}]+)\}', bus)[1]
    limits[name] = [int(v) for v in re.findall(r'(\d+)U', values)]
for lo, hi, cmd in windows:
    assert limits['minimumMs'][cmd] == lo and limits['maximumMs'][cmd] == hi
    assert lo <= widths[cmd] * 0.9 <= widths[cmd] * 1.1 <= hi, (lo, hi, cmd)
for left, right in zip(windows, windows[1:]):
    assert left[1] < right[0]
for ms in range(0, 600):
    matches = [cmd for lo, hi, cmd in windows if lo <= ms <= hi]
    assert len(matches) <= 1
for name, value in [('F_BATCH', 1), ('F_9VFAULT', 2), ('F_EMERG', 4), ('F_ACTIVITY', 8)]:
    assert int(re.search(rf'#define {name}\s+(0x[0-9A-F]+)', tiny)[1], 16) == value
assert 'parity ^= one' in tiny and 'uint8_t f = flags & 0x0F' in tiny
assert 'busDriveLow(); dms(60); busRelease(); dms(15);' in tiny
assert 'dms(one ? 30 : 10);' in tiny
assert 'sleep_enable();' in tiny and 'sleep_cpu();' in tiny
assert 'wdtOffEarly' in tiny and 'section(".init3")' in tiny
assert 'eeprom_update_byte(&eeInv, (uint8_t)~newv)' in tiny
assert '#ifndef ARDUINO' in tiny, 'standalone AVR CI entry point missing'
print('ATtiny v4 LINKFIX: decoder boundaries, flags, response/parity and sleep contract OK')
