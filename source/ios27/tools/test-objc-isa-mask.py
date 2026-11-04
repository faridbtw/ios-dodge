#!/usr/bin/env python3
"""Isolated 24B119 boot experiment: adapt libobjc ISA extraction to 38-bit VA.

Apple's runtime assumes at least 43 address bits when masking a signed ISA.
This BCM2837 kernel's standard-PAC configuration has 38 address bits. Keep
PAC enabled and test only the ISA extraction mask; original cache is untouched.
This is a build-specific experiment, not a general iOS compatibility claim.
"""
from pathlib import Path
import hashlib
import json
import struct

base = Path(__file__).resolve().parent
name = 'dyld_shared_cache_arm64e.01'
source = base / 'cache' / name
data = bytearray(source.read_bytes())
original = hashlib.sha256(data).hexdigest()
assert original == '20d634a13b3a849a6d834e1a466be5dee4245a98f0f210daafbbf22e89720822'
assert data[88:104].hex() == 'a78f816efae53aafa753766346190a44'
# libobjc __text, from the cache's Mach-O load commands. No other dylib touched.
start, end = 0x5240, 0x41c90
assert struct.unpack_from('<I', data, 0x1b494)[0] == 0x927d9d00
registers = 31 | (31 << 5)
opcodes = 3 << 29
pattern = 0x927d9d00 & ~(registers | opcodes)
changes = []
for off in range(start, end, 4):
    word = struct.unpack_from('<I', data, off)[0]
    if word & ~(registers | opcodes) != pattern:
        continue
    opcode = (word >> 29) & 3
    assert opcode in (0, 1, 3)
    if opcode == 1:
        assert (word >> 5) & 31 == 31, "ORR must be MOV mask alias"
    # AND/ANDS Xd,Xn,#0x7fffffffff8 -> AND Xd,Xn,#0x3ffffffff8.
    # N=1, immr=61 (rotate left 3), imms=39 -> 34.
    changed = (word & ~(63 << 10)) | (34 << 10)
    struct.pack_into('<I', data, off, changed)
    changes.append({'offset': off, 'va': hex(0x180400000 + off),
                    'before': hex(word), 'after': hex(changed)})
assert len(changes) == 302
target = base / 'sysroot-work/System/Cryptexes/OS/System/Library/Caches/com.apple.dyld' / name
temporary = target.with_suffix('.isa-test-tmp')
temporary.write_bytes(data)
temporary.chmod(0o644)
temporary.replace(target)  # break existing hardlinks; never mutate original cache
assert hashlib.sha256(source.read_bytes()).hexdigest() == original
receipt = {'experimental': True, 'build': '24B119', 'original_sha256': original,
           'patched_sha256': hashlib.sha256(data).hexdigest(),
           'original_unchanged': True, 'changes': changes,
           'result': 'not tested on phone yet'}
(base / 'OBJC-ISA-TEST.json').write_text(json.dumps(receipt, indent=2) + '\n')
print(f'Prepared {len(changes)} libobjc ISA-mask instructions in isolated sysroot')
