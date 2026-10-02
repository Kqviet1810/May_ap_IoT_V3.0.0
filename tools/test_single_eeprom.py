"""Host fault injection of production single-EEPROM driver and reminder persistence."""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--cxx', default='g++')
parser.add_argument('--sanitize', action='store_true')
args = parser.parse_args()
config = (root / 'MAYAP_INDUSTRIAL_v4_0_0/config.h').read_text(encoding='utf-8')
machine = (root / 'MAYAP_INDUSTRIAL_v4_0_0/machine_control.h').read_text(encoding='utf-8')

def body(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

constants = []
for name in ('EEPROM_I2C_ADDRESS', 'EEPROM_CAPACITY_BYTES', 'EEPROM_PAGE_SIZE',
             'EEPROM_MAX_WRITE_CHUNK', 'EEPROM_WRITE_TIMEOUT_MS', 'EEPROM_IO_RETRIES',
             'EEPROM_RETRY_GAP_MS', 'I2C_STORAGE_LOCK_TIMEOUT_MS', 'MAX_CUSTOM_REMINDERS',
             'CUSTOM_REMINDER_LABEL_LEN', 'EEPROM_ADDR_REMINDERS_A', 'EEPROM_ADDR_REMINDERS_B',
             'REMINDER_MAGIC', 'REMINDER_SCHEMA'):
    match = re.search(rf'constexpr\s+\w+\s+{name}\s*=.*?;', config + '\n' + machine)
    assert match, name
    constants.append(match.group())

parts = constants + [body(config, 'struct CustomReminder') + ';',
                     body(config, 'struct ReminderSet') + ';',
                     body(config, 'inline void sanitizeReminderSet('),
                     body(machine, 'inline uint32_t mcCrc32('), '#pragma pack(push, 1)']
for name in ('PackedReminderEntryV1', 'PackedReminderSetV1', 'ReminderRecordV1'):
    parts.append(body(machine, 'struct ' + name) + ';')
parts += ['#pragma pack(pop)', body(machine, 'inline PackedReminderSetV1 packReminders('),
          body(machine, 'inline ReminderSet unpackReminders('),
          body(machine, 'class ExternalEeprom24xx') + ';',
          'class ReminderStore { public: bool ready_ = true;']
for signature in ('bool loadReminders(', 'bool saveReminders(', 'static bool newer(',
                  'static bool validReminders(', 'bool refreshReminderCache('):
    parts.append(body(machine, signature))
parts += ['template <typename T> ' + body(machine, 'bool readRecord('),
          'template <typename T> ' + body(machine, 'bool writeRecord('),
          'ExternalEeprom24xx eeprom_; bool reminderCacheValid_ = false;',
          'bool reminderCurrentIsA_ = false; uint32_t reminderSequence_ = 0;',
          'PackedReminderSetV1 reminderPayload_{}; };']

with tempfile.TemporaryDirectory(prefix='mayap-single-eeprom-') as temporary:
    output = Path(temporary)
    (output / 'actual-single-eeprom.inc').write_text('\n'.join(parts), encoding='utf-8')
    executable = output / 'single-eeprom'
    flags = ['-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror']
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    subprocess.run([args.cxx, *flags, '-I', str(output),
                    str(root / 'tests/single-eeprom.cpp'), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
