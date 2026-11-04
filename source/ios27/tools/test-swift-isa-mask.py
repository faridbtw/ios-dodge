#!/usr/bin/env python3
"""Reproduce the build-specific Swift class-pointer mask experiment after ObjC."""
from pathlib import Path
import hashlib
import json
import struct

b = Path(__file__).resolve().parent
p = b / 'sysroot-work/System/Cryptexes/OS/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64e.01'
data = bytearray(p.read_bytes())
assert hashlib.sha256(data).hexdigest() == '75c6cf6a6965958edd8c04cd76cbae0bb2474b7420bcd5f7a434cbbc4de7c1b7'
receipt = json.loads((b / 'SWIFT-ISA-TEST.json').read_text())
assert len(receipt['changes']) == 9
for change in receipt['changes']:
    va = int(change['va'], 16)
    assert 0x181c73560 <= va < 0x182123740
    off = va - 0x180400000
    assert struct.unpack_from('<I', data, off)[0] == int(change['before'], 16)
    struct.pack_into('<I', data, off, int(change['after'], 16))
assert hashlib.sha256(data).hexdigest() == receipt['patched_sha256']
temporary = p.with_suffix('.swift-test-tmp')
temporary.write_bytes(data)
temporary.chmod(0o644)
temporary.replace(p)
print('Applied nine Swift class-pointer mask instructions in isolated sysroot')
