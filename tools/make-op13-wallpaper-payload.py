#!/usr/bin/env python3
"""Add on-demand wallpaper catalog services to the verified GPU RAM image.

The lock-screen poster switcher remains guarded: its hosted scene renders
black on this port. Wallpaper selection is tested through Settings instead.
"""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import runpy
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
APFS = ROOT / 'source/hackios-xnu-userland/tools/apfs'
sys.path.insert(0, str(APFS))
import apfs


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--preview-library', type=Path,
                   help='Scoped CoreGraphics replacement for the iOS20F66 Metal preview capture')
    a = p.parse_args()
    base = ROOT / 'experimental/gpu-integrated/v1'
    meta = json.loads((base / 'BUILD.json').read_text())
    original = (base / 'payload.bin').read_bytes()
    assert hashlib.sha256(original).hexdigest() == meta['payload_sha256']
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    shutil.copytree(base / 'root', out / 'root', symlinks=True)
    shutil.copy2(base / 'root.manifest', out / 'root.manifest')
    modified = []
    stock = ROOT / 'diagnostics/20261024/apple-stock-jobs/System/Library/LaunchDaemons'
    for label in ('com.apple.duetexpertd', 'com.apple.mobileassetd'):
        job = plistlib.loads((stock / (label + '.plist')).read_bytes())
        # Register stock Mach services on demand, without maintenance timers,
        # notifications, keepalive or unconditional boot-time execution.
        for key in ('RunAtLoad', 'KeepAlive', 'StartInterval', 'StartCalendarInterval',
                    'WatchPaths', 'QueueDirectories', 'StartOnMount', 'LaunchEvents'):
            job.pop(key, None)
        job.update(StandardErrorPath='/dev/console', StandardOutPath='/dev/console', ThrottleInterval=60)
        job.setdefault('EnvironmentVariables', {})['OS_ACTIVITY_DT_MODE'] = 'YES'
        if label == 'com.apple.duetexpertd':
            # SpringBoard already owns this endpoint in the RAM port.
            job['MachServices'].pop('com.apple.proactive.HomeScreenSuggestion.xpc', None)
        name = 'LaunchDaemons/' + label + '.plist'
        (out / 'root' / name).write_bytes(plistlib.dumps(job, fmt=plistlib.FMT_BINARY))
        with (out / 'root.manifest').open('a') as manifest:
            manifest.write('100644\t0\t0\t' + name + '\n')
        modified.append('/' + name)
    for source in (base / 'root').rglob('*'):
        if source.is_file() and not source.is_symlink():
            assert source.read_bytes() == (out / 'root' / source.relative_to(base / 'root')).read_bytes()
    if a.preview_library:
        library = a.preview_library.read_bytes()
        assert b'CoreGraphics preview installed' in library
        (out / 'root/lib/libredwood-wallpaper.dylib').write_bytes(library)
        with (out / 'root/etc/inject.conf').open('a') as config:
            config.write('\n/redwood/lib/libredwood-wallpaper.dylib PosterBoard\n')
        with (out / 'root.manifest').open('a') as manifest:
            manifest.write('100755\t0\t0\tlib/libredwood-wallpaper.dylib\n')
        modified.extend(['/lib/libredwood-wallpaper.dylib', '/etc/inject.conf'])
    subprocess.run([sys.executable, str(APFS / 'mkapfs.py'), '-z', '-o', str(out / 'root.apfs'),
                    f'redwood={out / "root"},manifest={out / "root.manifest"}'], check=True)
    volume = apfs.pick_volume(apfs.Container(str(out / 'root.apfs')), None)
    for name, _, oid, _ in apfs.walk(volume, 2):
        inode = volume.inode(oid)
        if inode.is_reg:
            assert inode.contents() == (out / 'root' / name.lstrip('/')).read_bytes(), name
    api = runpy.run_path(str(ROOT / 'tools/make-op13-test-payload.py'))
    entries = api['unpack'](original)
    entries['ramdisk'] = (out / 'root.apfs').read_bytes()
    blob = api['pack'](entries)
    assert api['unpack'](blob) == entries
    (out / 'payload.bin').write_bytes(blob)
    for key in ('validation', 'offline_validation'):
        meta.pop(key, None)
    meta.update(device_validated=False, hardware_acceleration_verified=False,
                source_payload_sha256=hashlib.sha256(original).hexdigest(),
                payload_sha256=hashlib.sha256(blob).hexdigest(),
                wallpaper_picker_fix_requested=True, wallpaper_picker_verified=False,
                coregraphics_preview_requested=bool(a.preview_library),
                lock_screen_picker_enabled=False, modified_root_files=modified,
                user_confirmed_scrolling=False, user_confirmed_battery=False, user_confirmed_slider=False)
    loader = ROOT / 'experimental/uefi-turtle/live-reset-v12/dodge-ios-uefi.img'
    if loader.exists():
        receipt = json.loads(loader.with_suffix('.json').read_text())
        if receipt.get('device_validated') and receipt.get('live_cold_reset_verified'):
            assert hashlib.sha256(loader.read_bytes()).hexdigest() == receipt['image_sha256']
            meta.update(loader=str(loader.relative_to(ROOT)), loader_sha256=receipt['image_sha256'])
    (out / 'BUILD.json').write_text(json.dumps(meta, indent=2) + '\n')
    print('Wallpaper RAM payload:', out / 'payload.bin')


if __name__ == '__main__':
    main()
