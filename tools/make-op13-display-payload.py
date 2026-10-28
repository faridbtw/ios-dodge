#!/usr/bin/env python3
"""Replace only the kernel in the verified GLES/wallpaper RAM payload."""
import argparse
import hashlib
import json
from pathlib import Path
import runpy

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--kernel', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    base = ROOT / 'experimental/wallpaper/picker-v3-guarded'
    blob = (base / 'payload.bin').read_bytes()
    assert hashlib.sha256(blob).hexdigest() == '56e2b211b28921de148cfc7446f67b5c49d0ca1e5276e7cfc214218db4e613b4'
    api = runpy.run_path(str(ROOT / 'tools/make-op13-test-payload.py'))
    entries = api['unpack'](blob)
    old = dict(entries)
    entries['kernel'] = a.kernel.read_bytes()
    assert b'op13-display: complete-frame shadow composition enabled' in entries['kernel']
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    assert not (out / 'payload.bin').exists(), 'Use a fresh output'
    result = api['pack'](entries)
    decoded = api['unpack'](result)
    assert decoded == entries
    assert all(decoded[k] == old[k] for k in old if k != 'kernel')
    (out / 'payload.bin').write_bytes(result)
    meta = json.loads((base / 'BUILD.json').read_text())
    for key in ('validation', 'offline_validation'):
        meta.pop(key, None)
    meta.update(device_validated=False, hardware_acceleration_verified=False,
                wallpaper_picker_verified=False, user_confirmed_settings_wallpaper_selection=False,
                source_payload_sha256=hashlib.sha256(blob).hexdigest(),
                payload_sha256=hashlib.sha256(result).hexdigest(),
                kernel_sha256=hashlib.sha256(entries['kernel']).hexdigest(),
                display_commit_fix_requested=True, display_tearing_fixed=False,
                changed_entries=['kernel'], ramdisk_and_cmdline_unchanged=True,
                modified_root_files=[])
    (out / 'BUILD.json').write_text(json.dumps(meta, indent=2) + '\n')
    print(out / 'payload.bin')


if __name__ == '__main__':
    main()
