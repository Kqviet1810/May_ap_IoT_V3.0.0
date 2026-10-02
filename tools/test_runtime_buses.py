"""Compile the actual I2C supervisor and UART class against fault-injection HALs."""
import argparse
import subprocess
import tempfile
import re
import os
from pathlib import Path

root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser()
parser.add_argument('--cxx', default='g++')
parser.add_argument('--sanitize', action='store_true')
parser.add_argument('--check-regression', action='store_true')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='mayap-runtime-') as temporary:
    out = Path(temporary)
    i2c = (root / 'MAYAP_INDUSTRIAL_v1_0_0/i2c_supervisor.h').read_text(encoding='utf-8')
    i2c = i2c.replace('#include "config.h"', '').replace('#include <Wire.h>', '')
    (out / 'actual-i2c.inc').write_text(i2c, encoding='utf-8')
    machine = (root / 'MAYAP_INDUSTRIAL_v1_0_0/machine_control.h').read_text(encoding='utf-8')
    start = machine.index('namespace SHT485Config {')
    end = machine.index('\n// ===', machine.index('class SHT485Industrial {', start))
    (out / 'actual-uart.inc').write_text(machine[start:end], encoding='utf-8')
    start = machine.index('  void updateAttinyLink(uint32_t now) {')
    end = machine.index('  void updateBatchTime(uint32_t now) {', start)
    (out / 'actual-attiny-controller.inc').write_text(machine[start:end], encoding='utf-8')
    services = (root / 'MAYAP_INDUSTRIAL_v1_0_0/service_recovery.h').read_text(encoding='utf-8')
    services = services.replace('#include "config.h"', '').replace('#include "runtime_recovery_policy.h"', '')
    (out / 'actual-services.inc').write_text(services, encoding='utf-8')
    network = (root / 'MAYAP_INDUSTRIAL_v1_0_0/network_service.h').read_text(encoding='utf-8')
    start = network.index('inline void mayapRequestWifiDeepRecovery()')
    end = network.index('inline void mayapSetWifiPortalOtaQuiesced', start)
    (out / 'actual-network.inc').write_text(network[start:end], encoding='utf-8')
    realtime = (root / 'MAYAP_INDUSTRIAL_v1_0_0/realtime_link.h').read_text(encoding='utf-8')
    transport = (root / 'MAYAP_INDUSTRIAL_v1_0_0/websocket_transport.h').read_text(encoding='utf-8')
    transport = '\n'.join(line for line in transport.splitlines() if not line.startswith('#include'))
    (out / 'actual-websocket-transport.inc').write_text(transport, encoding='utf-8')
    poll = (root / 'MAYAP_INDUSTRIAL_v1_0_0/esp_tls_async_poll.h').read_text(encoding='utf-8')
    poll = '\n'.join(line for line in poll.splitlines() if not line.startswith('#include'))
    (out / 'actual-esp-tls-poll.inc').write_text(poll, encoding='utf-8')
    ota = (root / 'MAYAP_INDUSTRIAL_v1_0_0/ota_update.h').read_text(encoding='utf-8')
    ota = '\n'.join(line for line in ota.splitlines() if not line.startswith('#include'))
    (out / 'actual-ota.inc').write_text(ota, encoding='utf-8')
    for name in ('attiny_bus', 'gpio_interrupts', 'serial_diagnostics', 'network_io_guard', 'bounded_http'):
        source = (root / ('MAYAP_INDUSTRIAL_v1_0_0/' + name + '.h')).read_text(encoding='utf-8')
        source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
        (out / ('actual-' + name + '.inc')).write_text(source, encoding='utf-8')
    cloud = (root / 'MAYAP_INDUSTRIAL_v1_0_0/cloud_alert_link.h').read_text(encoding='utf-8')
    start = cloud.index('struct OutboxItem {')
    end = cloud.index('inline void enqueueLevel(', start)
    (out / 'actual-cloud-outbox.inc').write_text(cloud[start:end], encoding='utf-8')
    start = cloud.index('inline void servicePinReset()')
    end = cloud.index('}  // namespace MayapCloudInternal', start)
    (out / 'actual-cloud-pin-reset.inc').write_text(cloud[start:end], encoding='utf-8')
    realtime = (root / 'MAYAP_INDUSTRIAL_v1_0_0/realtime_link.h').read_text(encoding='utf-8')
    start = realtime.index('inline void serviceEventLogPublish()')
    end = realtime.index('}  // namespace MayapRealtimeInternal', start)
    (out / 'actual-event-publish.inc').write_text(realtime[start:end], encoding='utf-8')
    start = realtime.index('inline void applyWifiPowerMode(')
    end = realtime.index('// --------------------------- Hop thu cau hinh/runtime', start)
    power = realtime[start:end]
    start = realtime.index('inline void serviceWifiPowerMode()')
    end = realtime.index('inline void serviceConfigPublish()', start)
    (out / 'actual-wifi-power.inc').write_text(power + realtime[start:end], encoding='utf-8')
    parts = []
    for begin, end in (('inline bool publishBootstrap(', 'struct TerminalResult {'),
                       ('inline void handleSessionMessage(', 'inline void realtimeMessageCallback('),
                       ('inline void serviceSessionTimeout(', '// realtime owner is'),
                       ('inline void serviceSnapshotPublish(', 'inline void serviceEventLogPublish(')):
        start = realtime.index(begin)
        stop = realtime.index(end, start)
        # The session callback is followed by other message helpers; extract its
        # balanced body only, so tests use the actual lease/sync implementation.
        if 'handleSessionMessage' in begin:
            brace = realtime.index('{', start)
            depth = 1
            stop = brace + 1
            while depth:
                depth += (realtime[stop] == '{') - (realtime[stop] == '}')
                stop += 1
        parts.append(realtime[start:stop])
    (out / 'actual-web-connect.inc').write_text('\n'.join(parts), encoding='utf-8')
    # Extract actual admission/confirmation/history implementations; no model copy.
    def function(source, signature):
        start = source.index(signature)
        brace = source.index('{', start)
        depth, end = 1, brace + 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        return source[start:end]
    hmi = (root / 'MAYAP_INDUSTRIAL_v1_0_0/hmi.h').read_text(encoding='utf-8')
    (out / 'actual-transaction-hmi.inc').write_text('\n'.join(function(hmi, sig) for sig in
        ('bool queueCommand(', 'bool startReminderSave(')), encoding='utf-8')
    start = realtime.index('struct PendingCommand {')
    stop = realtime.index('// -------------------------- Hop thu nhat ky', start)
    (out / 'actual-transaction-state.inc').write_text(realtime[start:stop], encoding='utf-8')
    (out / 'actual-transaction-dispatch.inc').write_text('\n'.join(function(realtime, sig) for sig in
        ('inline void handleCommandMessage(', 'inline void handleReminderSetMessage(',
         'inline void flushCompletedTransactions(', 'inline void expirePendingCommands(',
         'inline void serviceHistoryResponse(')), encoding='utf-8')
    start = realtime.index('struct TerminalResult {')
    stop = realtime.index('inline bool replayTerminal(', start)
    (out / 'actual-transaction-terminal.inc').write_text(realtime[start:stop] + '\n' +
        function(realtime, 'inline bool publishAck(const char *requestId') + '\n' +
        function(realtime, 'inline bool replayTerminal('), encoding='utf-8')
    (out / 'actual-transaction-confirm.inc').write_text('\n'.join(function(realtime, sig) for sig in
        ('inline void mayapWebConfirmCommand(', 'inline void mayapWebConfirmConfigSave(',
         'inline void mayapWebConfirmReminderSave(')), encoding='utf-8')
    json_candidates = [Path(os.environ.get('MAYAP_ARDUINOJSON', 'missing')),
                       Path.home() / 'Arduino/libraries/ArduinoJson/src',
                       Path.home() / 'Documents/Arduino/libraries/ArduinoJson/src']
    json_include = next((path for path in json_candidates if (path / 'ArduinoJson.h').is_file()), None)
    if json_include is None:
        raise SystemExit('ArduinoJson 7 required for actual retained bootstrap/session tests')
    cfg = (root / 'MAYAP_INDUSTRIAL_v1_0_0/config.h').read_text(encoding='utf-8')
    cadence_names = ('WEB_SNAPSHOT_ACTIVE_INTERVAL_MS', 'WEB_SNAPSHOT_IDLE_INTERVAL_MS')
    (out / 'actual-web-cadence-config.inc').write_text('\n'.join(
        re.search(r'constexpr [^;]*\b' + name + r'\b[^;]*;', cfg)[0]
        for name in cadence_names), encoding='utf-8')
    names = ('PIN_ATTINY_BUS', 'ATTINY_COMMAND_WIDTH_MS', 'ATTINY_BUS_MAX_RETRY',
             'ATTINY_MSG_MAX_COMMAND', 'ATTINY_MSG_STATUS_BASE', 'ATTINY_MSG_STATUS_MAX',
             'ATTINY_MSG_BATCH_START', 'ATTINY_MSG_BATCH_END', 'ATTINY_MSG_SIREN_ON',
             'ATTINY_MSG_SIREN_OFF', 'ATTINY_MSG_STATUS_QUERY', 'ATTINY_MSG_ACTIVITY_ON',
             'ATTINY_MSG_ACTIVITY_OFF', 'ATTINY_PROTOCOL_VERSION',
             'ATTINY_STATUS_FLAG_BATCH', 'ATTINY_STATUS_FLAG_9V_LOW',
             'ATTINY_STATUS_FLAG_SIREN', 'ATTINY_STATUS_FLAG_ACTIVITY',
             'ATTINY_ACTIVITY_OFF_CONFIRM_MS', 'ATTINY_STATUS_RESPONSE_TIMEOUT_MS',
             'ATTINY_9V_CONFIRM_MS', 'ATTINY_SIREN_REASSERT_MS', 'ATTINY_RESYNC_RETRY_MS',
             'ATTINY_STATUS_ARMED_INTERVAL_MS', 'ATTINY_STATUS_IDLE_INTERVAL_MS')
    declarations = [re.search(r'constexpr [^;]*\b' + name + r'\b[^;]*;', cfg)[0] for name in names]
    (out / 'actual-attiny-config.inc').write_text('\n'.join(declarations), encoding='utf-8')
    tiny = (root / 'ATTINY13A_POWER_ALARM/ATTINY13A_POWER_ALARM.ino').read_text(encoding='utf-8')
    decoder = re.search(r'static uint8_t decode\(uint16_t w\) \{[^}]*\}', tiny)[0]
    (out / 'actual-tiny-decoder.inc').write_text(decoder, encoding='utf-8')
    boot = (root / 'MAYAP_INDUSTRIAL_v1_0_0/boot_diagnostic.h').read_text(encoding='utf-8')
    mailbox = 'namespace MayapBootInternal { static volatile uint8_t homeReleased=0, operationsReady=0; }\n'
    for name in ('mayapBootHomeReleased', 'mayapBootReleaseHome', 'mayapBootOperationsReady',
                 'mayapBootAcknowledgeHomeFrame'):
        mailbox += re.search(r'inline (?:bool|void) ' + name + r'\(\) \{[^}]*\}', boot)[0] + '\n'
    (out / 'actual-boot-mailbox.inc').write_text(mailbox, encoding='utf-8')
    for test in ('runtime-buses', 'runtime-network', 'runtime-ota', 'runtime-attiny', 'runtime-attiny-state', 'runtime-stability', 'runtime-websocket', 'runtime-esp-tls-poll', 'runtime-web-connect', 'runtime-transactions', 'notes-link'):
        executable = out / (test + ('.exe' if __import__('os').name == 'nt' else ''))
        command = [args.cxx, '-std=c++11', '-Wall', '-Wextra', '-Werror', '-I', str(out),
                   str(root / ('tests/' + test + '.cpp')), '-o', str(executable)]
        if test in ('runtime-transactions','notes-link'): command[1] = '-std=c++17'
        if test in ('runtime-web-connect', 'runtime-transactions','notes-link'):
            command += ['-I', str(json_include)]
        if test == 'notes-link': command += ['-I', str(root / 'MAYAP_INDUSTRIAL_v1_0_0')]
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    if args.check_regression:
        # Demonstrate that the expanded test actually rejects the logged bug,
        # not merely that the patched source compiles. Only a temporary header
        # is mutated; production files and the Tiny sketch remain untouched.
        header = out / 'actual-attiny_bus.inc'
        source = header.read_text(encoding='utf-8')
        assert 'if (busHigh() && count >= RESPONSE_EDGES' in source
        header.write_text(source.replace('if (busHigh() && count >= RESPONSE_EDGES',
                                         'if (count >= RESPONSE_EDGES'), encoding='utf-8')
        executable = out / ('runtime-attiny-regression' + ('.exe' if __import__('os').name == 'nt' else ''))
        subprocess.run([args.cxx, '-std=c++11', '-Wall', '-Wextra', '-Werror', '-I', str(out),
                        str(root / 'tests/runtime-attiny.cpp'), '-o', str(executable)], check=True)
        regression = subprocess.run([str(executable)], capture_output=True, text=True)
        assert regression.returncode != 0, 'Missing HIGH guard was not detected'
        print('Regression proof: legal >30 ms final LOW fails without production HIGH guard, as expected')
        poll_header = out / 'actual-esp-tls-poll.inc'
        original_poll = poll_header.read_text(encoding='utf-8')
        rearm = 'FD_ZERO(&tls->wset); FD_SET(tls->sockfd, &tls->wset);'
        assert rearm in original_poll
        poll_header.write_text(original_poll.replace(rearm, 'FD_ZERO(&tls->wset);'), encoding='utf-8')
        executable = out / 'runtime-esp-tls-poll-regression'
        subprocess.run([args.cxx, '-std=c++11', '-Wall', '-Wextra', '-Werror', '-I', str(out),
                        str(root / 'tests/runtime-esp-tls-poll.cpp'), '-o', str(executable)], check=True)
        result = subprocess.run([str(executable)], capture_output=True, text=True)
        poll_header.write_text(original_poll, encoding='utf-8')
        assert result.returncode != 0, 'Missing TCP readiness re-arm was not detected'
        print('Regression proof: actual IDF TCP polling stalls without write fd_set re-arm, as expected')
        snapshot_header = out / 'actual-web-connect.inc'
        original_snapshot = snapshot_header.read_text(encoding='utf-8')
        edge_gate = '!forceSnapshotPublish && !lightChanged &&'
        assert edge_gate in original_snapshot
        snapshot_header.write_text(original_snapshot.replace(edge_gate,
            '!forceSnapshotPublish && (!lightChanged || true) &&'), encoding='utf-8')
        executable = out / 'runtime-web-connect-regression'
        subprocess.run([args.cxx, '-std=c++11', '-Wall', '-Wextra', '-Werror', '-I', str(out),
                        '-I', str(json_include), str(root / 'tests/runtime-web-connect.cpp'), '-o', str(executable)], check=True)
        result = subprocess.run([str(executable)], capture_output=True, text=True)
        snapshot_header.write_text(original_snapshot, encoding='utf-8')
        assert result.returncode != 0 and 'snapshots.size() == 3' in result.stderr, 'Missing lamp edge publication was not detected'
        print('Regression proof: stale forced sample delays actual lamp state without edge publication, as expected')
        # Each targeted mutation restores one of the review's actual failure
        # windows. Compilation must pass and the runtime assertions must fail.
        for name, replacement, label in (
            ('actual-transaction-hmi.inc',
             ('if (onAdmitted) onAdmitted(id, admissionContext);', 'if (onAdmitted) (void)admissionContext;'), 'command correlation after queue visibility'),
            ('actual-transaction-hmi.inc',
             ('reminderSave.readyForHost = !deferForHost;', 'reminderSave.readyForHost = true; (void)deferForHost;'), 'Reminder visible before transaction ID'),
            ('actual-transaction-terminal.inc',
             ('if (!received && !uncertain)', 'if (!received)'), 'uncertain timeout poisons terminal cache'),
            ('actual-transaction-dispatch.inc',
             ('if (!publishJson("history/reported", doc, false)) return;', 'publishJson("history/reported", doc, false);'), 'history advances on failed send')):
            target = out / name
            original = target.read_text(encoding='utf-8')
            assert replacement[0] in original
            target.write_text(original.replace(*replacement), encoding='utf-8')
            executable = out / 'runtime-transactions-regression'
            subprocess.run([args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(out),
                            '-I', str(json_include), str(root / 'tests/runtime-transactions.cpp'), '-o', str(executable)], check=True)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            target.write_text(original, encoding='utf-8')
            assert result.returncode != 0, 'Mutation not detected: ' + label
            print('Regression proof: rejected ' + label)
