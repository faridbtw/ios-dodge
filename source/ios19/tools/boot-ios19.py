#!/usr/bin/env python3
"""First iOS19 RAM boot. No flash, erase, partition or storage commands."""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import subprocess
import time

parser = argparse.ArgumentParser(description="Boot a prepared iOS19 profile in RAM only")
parser.add_argument('--profile', type=Path, required=True)
parser.add_argument('--serial', required=True)
parser.add_argument('--output', type=Path)
parser.add_argument('--check', action='store_true', help='Verify image hashes without contacting the phone')
options = parser.parse_args()
PROFILE = options.profile.expanduser().resolve()
BASE = PROFILE.parent
SERIAL = options.serial
OUT = options.output or BASE / ('phone-' + time.strftime('%Y%m%d-%H%M%S'))
receipts = []


def run(args, timeout=15, check=True):
    result = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
    receipts.append({'args': args, 'exit': result.returncode,
                     'output': result.stdout + result.stderr})
    (OUT / 'receipts.json').write_text(json.dumps(receipts, indent=2) + '\n')
    if check:
        result.check_returncode()
    return result.stdout + result.stderr


def fb(*args, serial='xnuloader', timeout=15, check=True):
    return run(['fastboot', '-s', serial, *args], timeout, check)


def wait_product(serial, product):
    for _ in range(40):
        try:
            if product in fb('getvar', 'product', serial=serial, timeout=2, check=False):
                return
        except subprocess.TimeoutExpired:
            pass
        time.sleep(.5)
    raise RuntimeError('Expected USB product did not respond: ' + product)


def upload_system(system_path):
    system = Path(system_path)
    total = system.stat().st_size
    chunk = OUT / 'system-chunk.tmp'
    try:
        with system.open('rb') as source:
            offset = 0
            while offset < total:
                amount = min(1024**3, total - offset)
                with chunk.open('wb') as dest:
                    left = amount
                    while left:
                        data = source.read(min(left, 8*1024**2))
                        if not data:
                            raise RuntimeError('Unexpected end of system image')
                        dest.write(data)
                        left -= len(data)
                fb('oem', 'ramdisk-chunk', format(offset, 'x'), format(total, 'x'))
                print(f'Uploading system to RAM: {offset/total:.0%} ({amount/1024**2:.0f} MiB)', flush=True)
                fb('stage', str(chunk), timeout=180)
                offset += amount
                (OUT / 'upload-progress.json').write_text(json.dumps({'written': offset, 'total': total})+'\n')
                print(f'System RAM upload: {offset/total:.0%}', flush=True)
    finally:
        chunk.unlink(missing_ok=True)


def main():
    preflight = json.loads(PROFILE.read_text())
    assert preflight.get('ram_only') is True
    assert preflight.get('phone_partition_writes') is False
    images = preflight['images']
    for name in ['uefi', 'payload', 'boot', 'system']:
        path = Path(images[name]['path'])
        assert path.stat().st_size == images[name]['bytes']
        with path.open('rb') as stream:
            assert hashlib.file_digest(stream, 'sha256').hexdigest() == images[name]['sha256']
    if options.check:
        print('All profile image hashes verified; phone not contacted')
        return
    OUT.mkdir(parents=True, exist_ok=False)
    print('Waiting for Android startup', flush=True)
    for _ in range(90):
        state = run(['adb', '-s', SERIAL, 'get-state'], check=False)
        if state.strip() == 'device':
            assert run(['adb', '-s', SERIAL, 'shell', 'getprop', 'ro.board.platform']).strip() == 'sun'
            if run(['adb', '-s', SERIAL, 'shell', 'getprop', 'sys.boot_completed']).strip() == '1':
                break
        time.sleep(1)
    else:
        raise RuntimeError('Android did not finish startup')
    print('Starting iOS19 Project Boot UEFI with fastboot boot', flush=True)
    run(['adb', '-s', SERIAL, 'reboot', 'bootloader'])
    wait_product(SERIAL, 'sun')
    fb('boot', images['uefi']['path'], serial=SERIAL, timeout=30)
    wait_product('xnuloader', 'xnuloader')
    fb('stage', images['payload']['path'], timeout=40)
    upload_system(images['system']['path'])
    if 'slider=1' in preflight.get('cmdline', '').split():
        fb('oem', 'op13-slider-probe', timeout=30)
    fb('oem', 'sm8750-touch-arm', timeout=30)
    print('Starting iOS19 kernel', flush=True)
    fb('oem', 'run', '0', timeout=30)
    for n in range(24):
        time.sleep(5)
        try:
            result = fb('getvar', 'result', timeout=5, check=False)
            print(result.strip(), flush=True)
            fb('oem', 'log', timeout=10, check=False)
            fb('get_staged', str(OUT / f'live-{n:02d}.log'), timeout=15, check=False)
            captured = OUT / f'live-{n:02d}.log'
            if captured.exists() and any(token in captured.read_text(errors='replace') for token in ['hakios: early exception', '\n== PANIC:']):
                print('Kernel startup fault captured; stopping diagnostic polling', flush=True)
                break
            if any(word in result.lower() for word in ['early-exception', 'returned', 'panic', 'hung']):
                break
        except subprocess.TimeoutExpired:
            print('USB query timed out; leaving RAM boot untouched', flush=True)
            break
    print('Diagnostic directory: ' + str(OUT), flush=True)


if __name__ == '__main__':
    main()
