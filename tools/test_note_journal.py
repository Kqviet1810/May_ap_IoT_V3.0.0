"""Test the production journal; only EEPROM I/O is fake."""
import pathlib, subprocess, tempfile
root=pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='mayap-note-journal-') as d:
    binary=pathlib.Path(d)/'journal'
    subprocess.run(['g++','-std=c++17','-O1','-g','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer',
                    str(root/'tests/note-journal.cpp'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
