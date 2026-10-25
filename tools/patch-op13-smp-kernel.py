#!/usr/bin/env python3
"""Apply five checked ARM64 instructions to the exact release kernel, for SM8750 only.

This is a RAM-test artifact, not a full XNU rebuild. The matching source fixes
are in the XNU snapshot. The binary uses SM8750's fixed 0x40000 GICv4 stride;
the source accepts gicr_stride=0x40000 for this board (VLPIS is hidden here),
with architectural discovery retained for generic boards.
Never apply this patch to any other kernel or board.
"""
import hashlib
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]
EXPECTED = '03b797e6922bd05d0b2a6796eb1d23b64ca799b2ff6adff4c197e4464e65d4b8'
source = ROOT / 'build-kernel-match.bin'
data = bytearray(source.read_bytes())
if hashlib.sha256(data).hexdigest() != EXPECTED:
    raise SystemExit('Refusing: release kernel SHA-256 does not match')
if struct.unpack_from('<I', data)[0] != 0xfeedfacf:
    raise SystemExit('Not a 64-bit Mach-O')
segments, pos = [], 32
for _ in range(struct.unpack_from('<I', data, 16)[0]):
    command, size = struct.unpack_from('<II', data, pos)
    if command == 0x19:
        name, va, length, offset, file_size = struct.unpack_from('<16sQQQQ', data, pos + 8)
        segments.append((name.rstrip(b'\0'), va, offset, file_size))
    pos += size

patches = [
    (0xfffffff00745b88c, 0x9140828b, 0x9141028b, 'GIC redistributor stride 0x20000 -> 0x40000'),
    (0xfffffff00744b214, 0x12003d08, 0x12005d08, 'ml_get_cpu_number: retain Aff2'),
    (0xfffffff00744b254, 0x12003c0b, 0x12005c0b, 'ml_get_cluster_number inline: retain Aff2'),
    (0xfffffff00744b2bc, 0x12003d29, 0x12005d29, 'ml_get_cpu_number_local inline: retain Aff2'),
    (0xfffffff00744b340, 0x12003d0b, 0x12005d0b, 'ml_get_cluster_number_local inline: retain Aff2'),
]
records = []
for address, old, new, reason in patches:
    matches = [(offset + address - va) for name, va, offset, size in segments
        if name == b'__TEXT_EXEC' and va <= address < va + size]
    if len(matches) != 1: raise SystemExit(f'No unique executable location for {address:x}')
    offset = matches[0]
    if struct.unpack_from('<I', data, offset)[0] != old:
        raise SystemExit(f'Instruction mismatch at {address:x}')
    struct.pack_into('<I', data, offset, new)
    records.append(dict(address=hex(address), file_offset=offset,
        original=hex(old), patched=hex(new), reason=reason))
out = ROOT / 'experimental/kernel-sm8750-smp.bin'
out.parent.mkdir(exist_ok=True)
out.write_bytes(data)
out.with_suffix('.json').write_text(json.dumps(dict(source_sha256=EXPECTED,
    patched_sha256=hashlib.sha256(data).hexdigest(), board='OnePlus13 SM8750 only',
    full_kernel_rebuild=False, patches=records), indent=2) + '\n')
print(out)
