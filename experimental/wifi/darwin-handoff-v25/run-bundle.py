#!/usr/bin/env python3
"""RAM boot a verified Dodge Wi-Fi preflight loader and capture its explicit query."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--serial', required=True)
p.add_argument('--loader', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--action', choices=['clocks', 'power', 'map', 'registers', 'cesta', 'phy', 'link', 'slider', 'audio-smmu', 'audio-smmu-clocks', 'audio-watchdog', 'audio-tcu'], default='clocks')
p.add_argument('--recover-idle-loader', action='store_true')
p.add_argument('--stage-wifi-firmware', action='store_true', help='Stage exact local Peach firmware in RAM before the link probe')
a = p.parse_args()
firmware = None
if a.stage_wifi_firmware:
    assert a.action == 'link'
    firmware = Path(__file__).resolve().parents[3] / 'diagnostics/20261029-wifi/native-reference/wifi-qdss-v5.private.bin'
    assert hashlib.sha256(firmware.read_bytes()).hexdigest() == '9f22e9beab0357f652f55e1f2c6ec5c8ed7869c42a03f2d1876ea86d88751ea9'
image = a.loader.resolve()
manifest = json.loads(image.with_suffix('.json').read_text())
assert manifest['image_sha256'] == hashlib.sha256(image.read_bytes()).hexdigest()
packaging = manifest
if manifest.get('loader_executables_unchanged'):
    assert manifest['changed_inner_ffs'] == ['7bb28b99-61bb-11d5-9a5d-0090273fc14d']
    assert manifest['other_inner_ffs_unchanged']
    root = Path(__file__).resolve().parents[3]
    base = (root / manifest['source']).resolve()
    assert base.is_relative_to(root.resolve())
    assert hashlib.sha256(base.read_bytes()).hexdigest() == manifest['source_sha256']
    packaging = json.loads(base.with_suffix('.json').read_text())
    assert packaging['image_sha256'] == manifest['source_sha256']
assert packaging['source_sha256'] == 'f26410ca510dcb7352f5c0b814ca435aaefe05bdc3ed1697ef5091b79d588cb8'
assert all(packaging[k] for k in ['other_ffs_unchanged', 'bootshim_unchanged', 'dtb_unchanged', 'ramdisk_and_signature_bytes_unchanged'])
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=False)
receipts = []


def command(args, timeout=10, check=True):
    try:
        r = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
        receipts.append({'command': args, 'exit': r.returncode, 'output': r.stdout + r.stderr})
        if check:
            r.check_returncode()
        return r.stdout + r.stderr, r.returncode
    except subprocess.TimeoutExpired:
        receipts.append({'command': args, 'timeout': timeout})
        raise
    finally:
        (out / 'receipts.json').write_text(json.dumps(receipts, indent=2) + '\n')


def fb(args, serial='xnuloader', timeout=10):
    return command(['fastboot', '-s', serial, *args], timeout)[0]


if a.recover_idle_loader:
    assert 'xnuloader' in fb(['getvar', 'product'])
    assert 'result: idle' in fb(['getvar', 'result'])
    fb(['reboot'])

for _ in range(120):
    identity, code = command(['adb', '-s', a.serial, 'shell',
                             'getprop ro.board.platform; getprop sys.boot_completed'], check=False)
    if code == 0 and identity.splitlines() == ['sun', '1']:
        break
    time.sleep(1)
else:
    raise SystemExit('Matched Android did not finish starting')
command(['adb', '-s', a.serial, 'reboot', 'bootloader'])
for _ in range(45):
    try:
        if 'sun' in fb(['getvar', 'product'], a.serial, 2):
            break
    except (subprocess.TimeoutExpired, subprocess.CalledProcessError):
        pass
    time.sleep(.5)
else:
    raise SystemExit('Matched Android bootloader absent')
fb(['boot', str(image)], a.serial, 20)
for _ in range(45):
    try:
        if 'xnuloader' in fb(['getvar', 'product'], timeout=3):
            break
    except (subprocess.TimeoutExpired, subprocess.CalledProcessError):
        pass
    time.sleep(.5)
else:
    raise SystemExit('RAM loader absent')
print('Diagnostic loader RAM booted; no XNU or network started', flush=True)
try:
    if firmware is not None:
        fb(['stage', str(firmware)], timeout=15)
    commands = {'slider': 'op13-slider-probe',
                'audio-smmu': 'op13-audio-smmu-query',
                'audio-smmu-clocks': 'op13-audio-smmu-clocks',
                'audio-watchdog': 'op13-audio-watchdog',
                'audio-tcu': 'op13-audio-tcu-on'}
    action = commands.get(a.action, 'op13-wifi-' + a.action)
    fb(['oem', action], timeout=45 if a.action == 'link' else 10)
finally:
    fb(['oem', 'log'])
    fb(['get_staged', str(out / 'loader.log')])
print('\n'.join(line for line in (out / 'loader.log').read_text(errors='replace').splitlines()
                if line.startswith(('op13-wifi-', 'op13-slider:', 'op13-audio-smmu:'))), flush=True)
