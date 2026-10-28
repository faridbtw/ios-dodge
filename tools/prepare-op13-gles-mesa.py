#!/usr/bin/env python3
"""Restore the paired Mesa26.2.5 GLES source from a verified upstream tarball."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--archive', type=Path, default=ROOT / 'build/mesa26-ios/mesa-26.2.5.tar.xz')
    p.add_argument('--output', type=Path, default=ROOT / 'build/mesa26-ios/mesa-26.2.5')
    a = p.parse_args()
    manifest = json.loads((ROOT / 'source/mesa26-op13/SOURCE-MANIFEST.json').read_text())
    with a.archive.open('rb') as stream:
        assert hashlib.file_digest(stream, 'sha256').hexdigest() == manifest['upstream_archive_sha256']
    out = a.output.resolve()
    assert out.name == 'mesa-26.2.5' and not out.exists(), 'Use a fresh mesa-26.2.5 directory'
    out.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(a.archive) as archive:
        assert all(Path(m.name).parts[0] == 'mesa-26.2.5' for m in archive.getmembers())
        archive.extractall(out.parent, filter='data')
    patch = ROOT / 'source/mesa26-op13/mesa-26.2.5-op13.patch'
    subprocess.run(['patch', '--batch', '--forward', '--dry-run', '-p1', '-i', str(patch)], cwd=out, check=True)
    subprocess.run(['patch', '--batch', '--forward', '-p1', '-i', str(patch)], cwd=out, check=True)
    for entry in manifest['changed_files']:
        data = (out / entry['path']).read_bytes()
        assert len(data) == entry['bytes'] and hashlib.sha256(data).hexdigest() == entry['sha256'], entry['path']
    assert not list(out.rglob('*.rej'))
    (out / '.op13-prepared').write_text('Verified consolidated OnePlus13 GLES source patch\n')
    print('Verified', len(manifest['changed_files']), 'changed/new source files:', out)


if __name__ == '__main__':
    main()
