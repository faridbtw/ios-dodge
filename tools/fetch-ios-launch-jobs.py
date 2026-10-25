#!/usr/bin/env python3
"""Read stock 20F66 launch jobs from Apple's IPSW using cached HTTP ranges.

Uses the bundle's pinned IPSW identity and APFS/UDIF readers. Does not download
the whole firmware, modify system.apfs, or write to the phone.
"""
import functools
import importlib.util
from pathlib import Path
import plistlib
import struct
import sys
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
USER = ROOT / 'source/hackios-xnu-userland'
OUT = ROOT / 'diagnostics/20261024/apple-stock-jobs'
CACHE = OUT / 'ranges'
CACHE.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(USER / 'tools/apfs'))
import apfs
import dmg

spec = importlib.util.spec_from_file_location('ipsw', USER / 'ios165/fetch-from-ipsw.py')
ipsw = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ipsw)


def fetch(start, end):
    import io
    cache = CACHE / f'{start}-{end}.bin'
    if cache.exists(): return io.BytesIO(cache.read_bytes())
    req = urllib.request.Request(ipsw.URL, headers={'Range': f'bytes={start}-{end}'})
    with urllib.request.urlopen(req, timeout=30) as response:
        if response.status != 206 or not response.headers.get('Content-Range', '').startswith(f'bytes {start}-{end}/'):
            raise ValueError('Server did not honor the exact HTTP range')
        data = response.read(end - start + 2)
    if len(data) != end - start + 1: raise ValueError('Short/oversized HTTP range')
    cache.write_bytes(data)
    return io.BytesIO(data)


ipsw.fetch = fetch
entry = next(e for e in ipsw.directory() if e[0] == '096-04088-077.dmg')
name, method, compressed, size, offset = entry
if method != 0 or size != compressed: raise SystemExit('DMG ZIP member is not stored; cannot range-read it')
header = fetch(offset, offset + 29).read()
nl, el = struct.unpack_from('<HH', header, 26)
base = offset + 30 + nl + el


class RemoteFile:
    def __init__(self): self.pos = 0
    def seek(self, offset, whence=0):
        self.pos = offset + (self.pos if whence == 1 else size if whence == 2 else 0)
        return self.pos
    def tell(self): return self.pos
    def read(self, length=-1):
        if length < 0: length = size - self.pos
        length = min(length, size - self.pos)
        if not length: return b''
        data = fetch(base + self.pos, base + self.pos + length - 1).read()
        self.pos += len(data)
        return data


dmg.open = lambda path, mode: RemoteFile()
disk = dmg.Dmg(name)
disk._chunk_data = functools.lru_cache(maxsize=32)(disk._chunk_data)
partition = next(p for p in dmg.read_gpt(disk) if p.type_guid == dmg.APFS_PART_TYPE)


class Partition:
    def read(self, offset, length): return disk.read(partition.offset + offset, length)


apfs.open_image = lambda path: Partition()
container = apfs.Container(name)
volume = apfs.pick_volume(container, None)
for directory in ('/System/Library/LaunchAngels', '/System/Library/LaunchDaemons'):
    oid, _ = volume.lookup(directory)
    for path, filename, inode_id, kind in apfs.walk(volume, oid, path=directory):
        if kind != 8 or not filename.endswith('.plist'): continue
        data = volume.inode(inode_id).contents()
        job = plistlib.loads(data)
        dest = OUT / directory.lstrip('/') / filename
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(data)
        if any(s in filename.lower() for s in ('poster', 'paper', 'media')):
            print(filename, job.get('Program') or job.get('ProgramArguments'), flush=True)
(OUT / 'SOURCE.txt').write_text(f'{ipsw.URL}\nmember={name}\nZIP method={method}\n'
    'Build identity: iOS 16.5 / 20F66, pinned by bundled ios165/fetch-from-ipsw.py\n')
print('Stock jobs saved to', OUT)
