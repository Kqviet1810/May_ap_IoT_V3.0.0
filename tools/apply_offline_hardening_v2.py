from pathlib import Path

base = Path(__file__).with_name("apply_offline_hardening.py")
source = base.read_text(encoding="utf-8")

old = '''s = replace_once(
    s,
    \'\'\'    testOutputMaskActive_ = 0U;\n    for (uint32_t &t : testOutputPulseUntil_) t = 0U;\n    testLimitPhase_ = TestLimitPhase::Idle;\'\'\',
    \'\'\'    testOutputMaskActive_ = 0U;\n    for (uint32_t &t : testOutputPulseUntil_) t = 0U;\n    testHeaterStartedAt_ = 0U;\n    testHeaterPostCoolUntil_ = 0U;\n    testTurnStartedAt_ = 0U;\n    testTurnOrigin_ = TrayPosition::Unknown;\n    testTurnDirection_ = TestOutputId::Count;\n    testLimitPhase_ = TestLimitPhase::Idle;\'\'\',
    "test enter reset state",
)'''

new = '''_enter_old = \'\'\'    testOutputMaskActive_ = 0U;\n    for (uint32_t &t : testOutputPulseUntil_) t = 0U;\n    testLimitPhase_ = TestLimitPhase::Idle;\'\'\'
_enter_new = \'\'\'    testOutputMaskActive_ = 0U;\n    for (uint32_t &t : testOutputPulseUntil_) t = 0U;\n    testHeaterStartedAt_ = 0U;\n    testHeaterPostCoolUntil_ = 0U;\n    testTurnStartedAt_ = 0U;\n    testTurnOrigin_ = TrayPosition::Unknown;\n    testTurnDirection_ = TestOutputId::Count;\n    testLimitPhase_ = TestLimitPhase::Idle;\'\'\'
if s.count(_enter_old) < 1:
    raise SystemExit("test enter reset state: no match")
s = s.replace(_enter_old, _enter_new, 1)'''

if source.count(old) != 1:
    raise SystemExit(f"wrapper could not patch v1 script; matches={source.count(old)}")
source = source.replace(old, new, 1)

# MachineRuntime chi cong bo co resumeConfirmationRequired; resumePending la
# state noi bo cua MachineController. HMI dung co cong khai nay de khoa cac
# setting service trong giai doan cho nguoi dung quyet dinh phuc hoi.
source = source.replace("currentRuntime.resumePending", "currentRuntime.resumeConfirmationRequired")

# Run the corrected one-shot patch in this process. __file__ is kept pointing
# at v1 so ROOT still resolves to the repository root exactly as designed.
namespace = {"__name__": "__main__", "__file__": str(base)}
exec(compile(source, str(base), "exec"), namespace, namespace)
