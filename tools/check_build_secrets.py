"""Repository must ship the named template, never filled build credentials."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
text = (root / 'MAYAP_INDUSTRIAL_v1_0_0/build_secrets.h').read_text(encoding='utf-8')
for name in ('MAYAP_OTA_PASSWORD',):
    match = re.search(rf'^#define {name}\s+"([^"\n]*)"\s*$', text, re.MULTILINE)
    if not match or match[1]:
        raise SystemExit('SECRETS FAIL: repository build_secrets.h must remain an empty template')
if re.search(r'MAYAP_MQTT_(USERNAME|PASSWORD)', text):
    raise SystemExit('SECRETS FAIL: obsolete fleet credential in template')
print('Build secrets: empty named template OK')
