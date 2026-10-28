#!/usr/bin/env python3
"""Exercise the actual panel command helper under ASAN/UBSAN."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
kernel=ROOT/'source/xnu-build-snapshot/xnu-8796.121.2/bsd/dev'
firmware=ROOT/'source/touch-runtime-v15'
assert (kernel/'op13_dsi_brightness.h').read_bytes()==(firmware/'op13_dsi_brightness.h').read_bytes()
with tempfile.TemporaryDirectory() as directory:
 binary=Path(directory)/'test-dsi'
 subprocess.run(['/usr/bin/clang','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
  '-I'+str(kernel),str(ROOT/'source/screen-brightness/test-dsi.c'),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
