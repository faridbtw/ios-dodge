#!/usr/bin/env python3
"""Read GPU firmware and physical display metadata from the authorized OnePlus.

No settings, sysfs files, partitions, or firmware on the phone are written.
The phone must be in Android with ADB available, not the synchronous XNU boot.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--wait', type=int, default=0)
    parser.add_argument('--output', type=Path,
        default=ROOT / 'diagnostics/20261025-adreno120/android')
    args = parser.parse_args()
    if not 0 <= args.wait <= 600: parser.error('--wait must be 0..600 seconds')
    deadline = time.monotonic() + args.wait
    adb = ['adb', '-s', args.serial]
    while True:
        result = subprocess.run(adb + ['get-state'], capture_output=True, timeout=5)
        if result.returncode == 0 and result.stdout.strip() == b'device': break
        if time.monotonic() >= deadline:
            raise SystemExit('Android ADB unavailable. Restart the current RAM boot into Android first.')
        time.sleep(2)
    platform = subprocess.check_output(adb + ['shell', 'getprop', 'ro.board.platform'], timeout=5).strip()
    if platform != b'sun': raise SystemExit('Refusing capture: expected SM8750 platform sun')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    records = []
    commands = {
        'identity': ['shell', 'getprop ro.product.model; getprop ro.soc.model; uname -a'],
        'gpu': ['shell', 'for n in gpu_model chipid gpuclk gpu_available_frequencies gpubusy; do '
            'echo "$n"; cat "/sys/class/kgsl/kgsl-3d0/$n" 2>/dev/null; done'],
        'panel': ['shell', 'for n in panel_id panel_name vendor display_mode; do '
            'echo "$n"; cat "/sys/kernel/oplus_display/$n" 2>/dev/null; done; '
            'for p in /sys/class/drm/card*-DSI*/modes /sys/class/drm/card*-DSI*/status; do '
            'echo "$p"; cat "$p" 2>/dev/null; done'],
        'display': ['shell', 'dumpsys', 'display'],
        'surfaceflinger': ['shell', 'dumpsys', 'SurfaceFlinger'],
    }
    for label, command in commands.items():
        try:
            result = subprocess.run(adb + command, capture_output=True, timeout=30)
            (out / (label + '.txt')).write_bytes(result.stdout)
            (out / (label + '.stderr.txt')).write_bytes(result.stderr)
            records.append({'capture': label, 'exit': result.returncode})
        except subprocess.TimeoutExpired:
            records.append({'capture': label, 'timeout': True})
    firmware = out / 'firmware'
    firmware.mkdir(exist_ok=True)
    for name in ['gen80000_gmu.bin', 'gen80000_sqe.fw', 'gen80000_aqe.fw', 'gen80000_zap.mbn']:
        result = subprocess.run(adb + ['pull', '/vendor/firmware/' + name, str(firmware / name)],
            capture_output=True, timeout=30)
        item = {'firmware': name, 'exit': result.returncode}
        path = firmware / name
        if result.returncode == 0 and path.is_file():
            item.update(size=path.stat().st_size, sha256=hashlib.sha256(path.read_bytes()).hexdigest())
        else: item['error'] = result.stderr.decode(errors='replace')
        records.append(item)
    result = subprocess.run(adb + ['pull', '/sys/firmware/fdt', str(out / 'device.dtb')],
        capture_output=True, timeout=30)
    records.append({'capture': 'live-fdt', 'exit': result.returncode,
        'stderr': result.stderr.decode(errors='replace')})
    (out / 'MANIFEST.json').write_text(json.dumps({'time_utc': datetime.now(timezone.utc).isoformat(),
        'platform': platform.decode(), 'phone_writes': False, 'records': records}, indent=2) + '\n')
    print('Read-only Android capture saved:', out)


if __name__ == '__main__':
    main()
