#!/usr/bin/env python3
from __future__ import annotations

import json
import re
import runpy
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"RELEASE SYNC FAIL: {message}")


def capture(text: str, pattern: str, label: str) -> str:
    match = re.search(pattern, text, re.MULTILINE)
    require(match is not None, f"khong tim thay {label}")
    return match.group(1)


manifest = json.loads(read("release-manifest.json"))
config = read("MAYAP_INDUSTRIAL_v4_0_0/config.h")
tiny = read("ATTINY13A_POWER_ALARM/ATTINY13A_POWER_ALARM.ino")
sw = read("sw.js")
build = read(".github/workflows/build-firmware.yml")
deploy = read(".github/workflows/deploy-cloudflare-worker.yml")
reliability = read(".github/workflows/reliability-checks.yml")

fw = capture(config, r'MAYAP_FIRMWARE_VERSION\[\]\s*=\s*"([^"]+)"', "firmware version")
hmi = capture(config, r'HMI_FIRMWARE_VERSION\[\]\s*=\s*"([^"]+)"', "HMI version")
esp_proto = capture(config, r'ATTINY_PROTOCOL_VERSION\s*=\s*(\d+)U', "ESP ATtiny protocol")
tiny_proto = capture(tiny, r'PROTOCOL_VERSION\s*=\s*(\d+)U', "ATtiny protocol")
web = capture(sw, r"const\s+CACHE\s*=\s*'mayap-web-v([^']+)'", "web cache version")

require(fw == manifest["firmware"], f"firmware {fw} != manifest {manifest['firmware']}")
require(hmi == manifest["hmi"], f"HMI {hmi} != manifest {manifest['hmi']}")
require(int(esp_proto) == int(manifest["attiny_protocol"]), "ESP ATtiny protocol lech manifest")
require(int(tiny_proto) == int(manifest["attiny_protocol"]), "ATtiny protocol lech manifest")
require(web == manifest["web"], f"web cache {web} != manifest {manifest['web']}")
require(manifest["release"] == manifest["firmware"], "release va firmware phai dong bo")
runpy.run_path(str(ROOT / "tools/check_build_secrets.py"), run_name="__main__")

require(f"esp32:esp32@{manifest['esp32_core']}" in build, "ESP32 core trong workflow lech manifest")
require(f"ARDUINO_CLI_VERSION: '{manifest['arduino_cli']}'" in build, "Arduino CLI trong workflow lech manifest")
require("actions/checkout@v7" in build, "build workflow chua dung checkout@v7")
require("actions/setup-node@v7" in build, "build workflow chua dung setup-node@v7")
require("actions/upload-artifact@v6" in build, "build workflow chua dung upload-artifact@v6")
require("softprops/action-gh-release@v3" in build, "release workflow chua dung action-gh-release@v3")
require("arduino/setup-arduino-cli" not in build, "khong dung setup-arduino-cli Node20 trong build")
require("execute offline hardening" not in build, "one-shot hardening cu van con trong build")
require("HEAD:hardening/v3.8.0" not in build, "build van co duong push nguoc v3.8.0")

for label, workflow in (("deploy", deploy), ("reliability", reliability)):
    require("actions/checkout@v7" in workflow, f"{label} chua dung checkout@v7")
    require("actions/setup-node@v7" in workflow, f"{label} chua dung setup-node@v7")
    require("node-version: '24'" in workflow, f"{label} chua pin Node 24")

require("check_v381_reliability.py" in build, "release path co the bo qua reliability gate")
require("check_release_sync.py" in build, "build path co the bo qua release sync gate")
require("check_release_sync.py" in deploy, "deploy path co the bo qua release sync gate")

# v3.8.3: release/build/deploy cannot bypass EEPROM history invariants.
# Run the dedicated checker from this existing gate so every current CI path
# inherits it without duplicating workflow plumbing.
if manifest["firmware"] >= "3.8.3":
    runpy.run_path(str(ROOT / "tools/check_eeprom_history.py"), run_name="__main__")

print(
    "Release sync OK: "
    f"release={manifest['release']} fw={fw} hmi={hmi} web={web} "
    f"attiny=v{esp_proto} esp32-core={manifest['esp32_core']} cli={manifest['arduino_cli']}"
)
