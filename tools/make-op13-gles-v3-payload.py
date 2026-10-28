#!/usr/bin/env python3
"""Package isolated Adreno830 GLES with the working battery/slider RAM root."""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
APFS = ROOT / 'source/hackios-xnu-userland/tools/apfs'
sys.path.insert(0, str(APFS))
import apfs


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--archive-root', type=Path, required=True)
    p.add_argument('--kernel', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    donor = a.archive_root.resolve()
    plan = json.loads((donor / 'package/runtime-plan.json').read_text())
    base = ROOT / 'experimental/slider/live-original-compositor-v1'
    original = (base / 'payload.bin').read_bytes()
    assert sha(original) == 'e7965f334f65401b39411dad1ddd9674b6076f92f124542c3a31a12e7a9132b2'
    image = donor / 'package/root.apfs'
    assert sha(image.read_bytes()) == next(e['sha256'] for e in plan['entries'] if e['name'] == 'ramdisk')
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    shutil.copytree(base / 'root', out / 'root', symlinks=True)
    shutil.copy2(base / 'root.manifest', out / 'root.manifest')
    v = apfs.pick_volume(apfs.Container(str(image)), None)
    changed = []
    with (out / 'root.manifest').open('a') as manifest:
        for name, _, oid, _ in apfs.walk(v, 2):
            if name not in ('/lib/libOSMesa.8.dylib', '/lib/libredwood-gl.dylib') and not name.startswith('/fw/'):
                continue
            inode = v.inode(oid)
            assert inode.is_reg
            target = out / 'root' / name.lstrip('/')
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(inode.contents())
            target.chmod(inode.mode & 0o777)
            manifest.write(f'{inode.mode:o}\t0\t0\t{name.lstrip("/")}\n')
            changed.append(name)
    # Keep the existing slider-aware injection libraries and every other file.
    for source in (base / 'root').rglob('*'):
        rel = source.relative_to(base / 'root')
        if source.is_file() and not source.is_symlink() and '/' + str(rel) not in changed:
            assert source.read_bytes() == (out / 'root' / rel).read_bytes(), str(rel)
    subprocess.run([sys.executable, str(APFS / 'mkapfs.py'), '-z', '-o', str(out / 'root.apfs'),
                    f'redwood={out / "root"},manifest={out / "root.manifest"}'], check=True)
    volume = apfs.pick_volume(apfs.Container(str(out / 'root.apfs')), None)
    for name, _, oid, _ in apfs.walk(volume, 2):
        inode = volume.inode(oid)
        if inode.is_reg:
            assert inode.contents() == (out / 'root' / name.lstrip('/')).read_bytes(), name
    api = runpy.run_path(str(ROOT / 'tools/make-op13-test-payload.py'))
    entries = api['unpack'](original)
    old = dict(t.split('=', 1) for t in entries['cmdline'].rstrip(b'\0').decode().split())
    args = dict(t.split('=', 1) for t in plan['cmdline'].split())
    # These omitted parameters default to exactly the v3 values. Retain touch
    # stage4 and button stage5, which have different defaults.
    for key in ('adreno8_gmuclk', 'adreno8_return', 'touch_poll_us'):
        args.pop(key)
    args.update({k: old[k] for k in ('op13_batt', 'slider', 'slider_power', 'slider_cal')})
    args.update(op13_usb='1', epoch=str(int(time.time())))
    cmdline = ' '.join(k + '=' + v for k, v in args.items())
    assert len(cmdline.encode()) < 416, len(cmdline)
    kernel = a.kernel.read_bytes()
    assert all(marker in kernel for marker in (b'oneplus-gpu:', b'op13-slider:', b'op13-battery:', b'GPU startup probes require'))
    entries.update(kernel=kernel, ramdisk=(out / 'root.apfs').read_bytes(), cmdline=cmdline.encode() + b'\0')
    payload = api['pack'](entries)
    assert api['unpack'](payload) == entries
    (out / 'payload.bin').write_bytes(payload)
    loader = ROOT / 'experimental/uefi-turtle/stable-overlay-v11/dodge-ios-uefi.img'
    build = dict(experimental=True, device_validated=False, hardware_acceleration_requested=True,
                 hardware_acceleration_verified=False, renderer='Mesa 26.2.5 freedreno Adreno830 GLES',
                 slider=True, live_battery_enabled=True, cpu_count_requested=7, firmware_usb_cpu=0,
                 loader=str(loader.relative_to(ROOT)), loader_sha256=sha(loader.read_bytes()),
                 system_sha256=plan['system_sha256'], payload_sha256=sha(payload), kernel_sha256=sha(kernel),
                 source_payload_sha256=sha(original), donor_root_sha256=sha(image.read_bytes()),
                 cmdline=cmdline, cmdline_bytes=len(cmdline), modified_root_files=sorted(changed),
                 phone_partition_writes=False, cold_reset_required=True, sound_work='paused')
    (out / 'BUILD.json').write_text(json.dumps(build, indent=2) + '\n')
    print('GLES RAM payload:', out / 'payload.bin', 'cmdline bytes:', len(cmdline))


if __name__ == '__main__':
    main()
