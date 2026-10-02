"""Repository must ship the named template, never filled MQTT credentials."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
text = (root / 'MAYAP_INDUSTRIAL_v4_0_0/build_secrets.h').read_text(encoding='utf-8')
for name in ('MAYAP_MQTT_USERNAME', 'MAYAP_MQTT_PASSWORD', 'MAYAP_OTA_PASSWORD'):
    match = re.search(rf'^#define {name}\s+"([^"\n]*)"\s*$', text, re.MULTILINE)
    if not match or match[1]:
        raise SystemExit('SECRETS FAIL: repository build_secrets.h must remain an empty template')
print('Build secrets: empty named template OK')
