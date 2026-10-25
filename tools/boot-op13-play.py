#!/usr/bin/env python3
"""RAM-boot an interactive, untimed iOS payload after matched Android reconnects."""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--serial', required=True)
p.add_argument('--payload', required=True, type=Path)
p.add_argument('--output', required=True, type=Path)
p.add_argument('--wait-seconds', type=int, default=300)
a = p.parse_args()
payload = a.payload.resolve()
payload_hash = hashlib.sha256(payload.read_bytes()).hexdigest()
original = payload == ROOT / 'touch-v15-payload.bin' and payload_hash == 'ca685f4f279b94fe9dba18999b3934570a1b86913b238425b5ea36877d5b7c8d'
loader = ROOT / ('dodge-known-v31-usb-loader.img' if original else 'experimental/adreno120/dodge-display-query.img')
entries = runpy.run_path(str(ROOT / 'tools/make-op13-test-payload.py'))['unpack'](payload.read_bytes())
boot = dict(t.split('=', 1) for t in entries['cmdline'].rstrip(b'\0').decode().split())
build = ({'payload_sha256': payload_hash, 'cmdline': entries['cmdline'].rstrip(b'\0').decode()}
         if original else json.loads((payload.parent / 'BUILD.json').read_text()))
assert build['payload_sha256'] == payload_hash
loader_hash = ('f26410ca510dcb7352f5c0b814ca435aaefe05bdc3ed1697ef5091b79d588cb8' if original
               else '29f2d04a2db4a684d6436564e3f3a916fbde01bb7081a9604882665b4052a86e')
assert hashlib.sha256(loader.read_bytes()).hexdigest() == loader_hash
assert boot['loader_return_after'] == '0' and boot['cpus'] == ('1' if original else '8')
assert all(boot.get(k) == '0' for k in ['gpu', 'adreno', 'metal', 'ufs', 'ufs_write', 'var_store', 'cpufreq'])
assert not any(k in boot for k in ['adreno8', 'sm8750_test_reset_seconds'])
assert len(entries['cmdline'].rstrip(b'\0')) <= 416
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=False)
receipts = []

def command(argv, timeout=10, check=True):
    r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
    receipts.append({'args': argv, 'exit': r.returncode, 'output': r.stdout + r.stderr})
    (out / 'receipts.json').write_text(json.dumps(receipts, indent=2) + '\n')
    if check:
        r.check_returncode()
    return r.stdout + r.stderr, r.returncode

print('Waiting for Android after physical restart', flush=True)
deadline = time.monotonic() + a.wait_seconds
while time.monotonic() < deadline:
    state, code = command(['adb', '-s', a.serial, 'get-state'], check=False)
    if code == 0 and state.strip() == 'device':
        board, _ = command(['adb', '-s', a.serial, 'shell', 'getprop ro.board.platform'])
        if board.strip() != 'sun':
            raise SystemExit('Unexpected board; no reboot attempted')
        ready, _ = command(['adb', '-s', a.serial, 'shell', 'getprop sys.boot_completed'])
        if ready.strip() == '1':
            break
    time.sleep(2)
else:
    raise SystemExit('Android did not reconnect; no boot attempted')

def fb(args, serial='xnuloader', timeout=10):
    # Fixed RAM staging/boot/start operations only; no partition writes.
    return command(['fastboot', '-s', serial, *args], timeout)[0]

print('Android connected; starting RAM boot', flush=True)
command(['adb', '-s', a.serial, 'reboot', 'bootloader'])
for _ in range(45):
    try:
        if 'sun' in fb(['getvar', 'product'], a.serial, 2):
            break
    except (subprocess.TimeoutExpired, subprocess.CalledProcessError):
        pass
    time.sleep(.5)
else:
    raise SystemExit('Matched bootloader did not enumerate')
fb(['boot', str(loader)], a.serial, 20)
for _ in range(50):
    try:
        if 'xnuloader' in fb(['getvar', 'product'], timeout=3):
            break
    except (subprocess.TimeoutExpired, subprocess.CalledProcessError):
        pass
    time.sleep(.5)
else:
    raise SystemExit('RAM loader did not enumerate')
fb(['stage', str(payload)], timeout=20)
fb(['oem', 'ramdisk'])
print('Uploading iOS system image into RAM', flush=True)
fb(['stage', str(ROOT / 'system.apfs')], timeout=120)
fb(['oem', 'sm8750-touch-arm'], timeout=15)
fb(['oem', 'run', '0'])
(out / 'RESULT.json').write_text(json.dumps({
    'ram_boot_started': True, 'automatic_return_seconds': 0,
    'hardware_acceleration': False, 'gpu_diagnostic_enabled': False,
    'phone_partition_writes': False, 'cmdline': build['cmdline'],
    'payload_sha256': build['payload_sha256'],
}, indent=2) + '\n')
print('Interactive iOS boot started; no automatic return timer', flush=True)
