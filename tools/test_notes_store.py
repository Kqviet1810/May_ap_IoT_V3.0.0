"""Actual AT24C512 driver + portable notes store, including every power-cut byte."""
import subprocess, tempfile, re
from pathlib import Path
root=Path(__file__).resolve().parents[1]
config=(root/'MAYAP_INDUSTRIAL_v1_0_0/config.h').read_text()
machine=(root/'MAYAP_INDUSTRIAL_v1_0_0/machine_control.h').read_text()
def body(signature):
    start=machine.index(signature);opening=machine.index('{',start);depth=1;end=opening+1
    while depth:
        depth+=(machine[end]=='{')-(machine[end]=='}');end+=1
    return machine[start:end]
constants=[]
for name in ('EEPROM_I2C_ADDRESS','EEPROM_CAPACITY_BYTES','EEPROM_PAGE_SIZE','EEPROM_MAX_WRITE_CHUNK','EEPROM_WRITE_TIMEOUT_MS','EEPROM_IO_RETRIES','EEPROM_RETRY_GAP_MS','I2C_STORAGE_LOCK_TIMEOUT_MS'):
    constants.append(re.search(rf'constexpr\s+\w+\s+{name}\s*=.*?;',config).group())
with tempfile.TemporaryDirectory(prefix='mayap-notes-store-') as tmp:
    out=Path(tmp);(out/'actual-notes-driver.inc').write_text('\n'.join(constants+[body('class ExternalEeprom24xx')+';']))
    exe=out/'notes-store'
    subprocess.run(['g++','-std=c++17','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I',tmp,'-I',str(root/'MAYAP_INDUSTRIAL_v1_0_0'),str(root/'tests/notes-store.cpp'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
