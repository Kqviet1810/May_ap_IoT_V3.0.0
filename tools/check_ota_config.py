from pathlib import Path
import json
import re

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "MAYAP_INDUSTRIAL_v1_0_0"
PUBLIC = FW / "build_public.h"
CONFIG = FW / "config.h"
WORKFLOW = ROOT / ".github/workflows/build-firmware.yml"

for obsolete in (FW / "build_secrets.h", FW / "build_secrets.local.h"):
    if obsolete.exists():
        raise SystemExit(f"OTA CONFIG FAIL: obsolete override exists: {obsolete.name}")

public = PUBLIC.read_text(encoding="utf-8")
matches = re.findall(r'^#define MAYAP_OTA_PASSWORD\s+("(?:[^"\\]|\\.)*")\s*$', public, re.MULTILINE)
if len(matches) != 1:
    raise SystemExit("OTA CONFIG FAIL: MAYAP_OTA_PASSWORD must be defined exactly once in build_public.h")
if json.loads(matches[0]):
    raise SystemExit("OTA CONFIG FAIL: tracked build_public.h must keep MAYAP_OTA_PASSWORD empty")

definitions = []
for path in FW.iterdir():
    if path.suffix in {".h", ".ino"} and re.search(
        r'^\s*#\s*define\s+MAYAP_OTA_PASSWORD\b',
        path.read_text(encoding="utf-8", errors="ignore"), re.MULTILINE):
        definitions.append(path.name)
if definitions != ["build_public.h"]:
    raise SystemExit(f"OTA CONFIG FAIL: expected one OTA definition in build_public.h, found {definitions}")

config = CONFIG.read_text(encoding="utf-8")
workflow = WORKFLOW.read_text(encoding="utf-8")
if "build_secrets" in config or "build_secrets" in workflow:
    raise SystemExit("OTA CONFIG FAIL: build_secrets dependency reintroduced")
if 'Path("MAYAP_INDUSTRIAL_v1_0_0/build_public.h")' not in workflow:
    raise SystemExit("OTA CONFIG FAIL: CI no longer injects OTA credential into build_public.h")
print("OTA config: single build_public.h source OK")
