#!/usr/bin/env python3
"""Validate the actual linked kernel's chained fixups before the UEFI signs it."""
from pathlib import Path
import hashlib
import json
import struct

base = Path(__file__).resolve().parent
project = base.parents[2]
data = (project / 'build/mac-xnu-ios27-v2/obj/mach.development.bcm2837').read_bytes()
header = struct.unpack_from('<8I', data)
assert header[0] == 0xfeedfacf
position = 32
segments, keys = [], []
fixups = None
for _ in range(header[4]):
    command, size = struct.unpack_from('<II', data, position)
    assert size >= 8 and position + size <= 32 + header[5]
    if command == 25:
        segment = struct.unpack_from('<II16sQQQQIIII', data, position)
        segments.append({'name': segment[2].split(b'\0')[0].decode(),
                         'vm': segment[3], 'vmsize': segment[4],
                         'fileoff': segment[5], 'filesize': segment[6]})
        assert segment[5] + segment[6] <= len(data)
        for n in range(segment[9]):
            section = position + 72 + 80*n
            assert section + 80 <= position + size
            if data[section:section+16].split(b'\0')[0] == b'__pac_keys':
                keys.append(struct.unpack_from('<QQ', data, section+32))
    if command == 0x80000034:
        fixups = struct.unpack_from('<II', data, position+8)
    position += size
assert fixups and keys and keys[0][1] >= 80
offset, size = fixups
assert offset + size <= len(data)
fixup_header = struct.unpack_from('<7I', data, offset)
assert fixup_header[4] == 0, 'Standalone kernel cannot have imports'
starts = offset + fixup_header[1]
image_base = next(s['vm'] for s in segments if s['name'] == '__TEXT')
seen = set()
authenticated = plain = 0
for index in range(struct.unpack_from('<I', data, starts)[0]):
    relative = struct.unpack_from('<I', data, starts+4+4*index)[0]
    if not relative:
        continue
    address = starts + relative
    length, page_size, pointer_format, segment_offset, _, page_count = struct.unpack_from('<IHHQIH', data, address)
    assert pointer_format == 7 and address+length <= offset+size
    segment = next(s for s in segments if s['vmsize'] and s['vm']-image_base == segment_offset)
    pages = struct.unpack_from('<'+'H'*((length-22)//2), data, address+22)
    assert page_count <= len(pages)
    for page in range(page_count):
        first = pages[page]
        if first == 0xffff:
            continue
        entries = []
        if first & 0x8000:
            entry = first & 0x7fff
            while True:
                assert entry < len(pages)
                value = pages[entry]
                entry += 1
                entries.append(value & 0x7fff)
                if value & 0x8000:
                    break
        else:
            entries = [first]
        for entry in entries:
            entry += page*page_size
            while True:
                assert entry+8 <= segment['filesize']
                slot = segment['fileoff']+entry
                assert slot not in seen
                seen.add(slot)
                raw = struct.unpack_from('<Q', data, slot)[0]
                assert not ((raw >> 62) & 1), 'Unexpected import binding'
                authenticated += (raw >> 63) & 1
                plain += not ((raw >> 63) & 1)
                stride = (raw >> 51) & 0x7ff
                if not stride:
                    break
                entry += 4*stride
receipt = {'kernel_sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data),
           'cpu_subtype': hex(header[2]), 'authenticated_fixups': authenticated,
           'plain_rebases': plain, 'imports': 0, 'all_chain_bounds_valid': True,
           'keys_section_bytes': keys[0][1]}
(base / 'KERNEL-FIXUP-VALIDATION.json').write_text(json.dumps(receipt, indent=2)+'\n')
print(json.dumps(receipt))
