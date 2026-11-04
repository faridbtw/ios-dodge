#!/usr/bin/env python3
"""Change only the displayed iPhone model name in a copy of the tested RAM UEFI."""
import argparse
import gzip
import hashlib
import importlib.util
import json
import lzma
from pathlib import Path
import struct
import zlib

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('packer', ROOT / 'tools/pack-op13-diagnostic-loader.py')
p = importlib.util.module_from_spec(spec)
spec.loader.exec_module(p)
sha = lambda b: hashlib.sha256(b).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    source = args.source.resolve()
    receipt = json.loads(source.with_suffix('.json').read_text())
    image = source.read_bytes()
    assert sha(image) == receipt['image_sha256']
    assert image[:8] == b'ANDROID!' and struct.unpack_from('<I', image, 40)[0] == 4
    kernel_size = struct.unpack_from('<I', image, 8)[0]
    decoder = zlib.decompressobj(31)
    raw = bytearray(decoder.decompress(image[4096:4096 + kernel_size]) + decoder.flush())
    assert decoder.eof
    suffix = decoder.unused_data
    old_raw = bytes(raw)
    outer_files = list(p.files(raw, 112))
    guided = [(g, f, n, s, z) for g, f, n in outer_files
              for k, s, z in p.sections(raw, f + 24, f + n)
              if k == 2 and raw[s + 4:s + 20] == p.LZMA_GUID]
    assert len(guided) == 1
    outer_guid, fpos, fsize, sec, slen = guided[0]
    offset = struct.unpack_from('<H', raw, sec + 20)[0]
    assert offset == 24 and sec == fpos + 24
    compressed = bytes(raw[sec + offset:sec + slen])
    original = lzma.decompress(compressed, format=lzma.FORMAT_ALONE)
    inner = bytearray(original)
    changed = []
    old_name, new_name = b'iPhone 17 Pro Max\0', b'iPhone 15 Pro Max\0'
    assert len(old_name) == len(new_name)
    for guid, pos, size in p.files(inner, 8):
        p.check_ffs(inner, pos, size)
        if guid not in p.TARGETS:
            continue
        pe_sections = [(s, n) for kind, s, n in p.sections(inner, pos + 24, pos + size) if kind == 16]
        assert len(pe_sections) == 1
        at, n = pe_sections[0]
        content = bytes(inner[at + 4:at + n])
        assert content[:2] == b'MZ' and content.count(old_name) == 2
        replacement = content.replace(old_name, new_name)
        assert sum(a != b for a,b in zip(content,replacement)) == 2
        inner[at + 4:at + n] = replacement
        p.fix_ffs(inner, pos, size)
        p.check_ffs(inner, pos, size)
        changed.append(guid)
    assert set(changed) == p.TARGETS
    for guid, pos, size in p.files(inner, 8):
        if guid not in p.TARGETS:
            assert inner[pos:pos + size] == original[pos:pos + size]
    prop = compressed[0]
    filters = [{'id': lzma.FILTER_LZMA1, 'dict_size': int.from_bytes(compressed[1:5], 'little'),
                'lc': prop % 9, 'lp': (prop // 9) % 5, 'pb': (prop // 9) // 5}]
    packed = bytearray(lzma.compress(bytes(inner), format=lzma.FORMAT_ALONE, filters=filters))
    packed[5:13] = len(inner).to_bytes(8, 'little')
    assert lzma.decompress(packed, format=lzma.FORMAT_ALONE) == inner
    tail = list(p.sections(raw, p.align(sec + slen, 4), fpos + fsize))
    assert len(tail) == 1 and tail[0][0] == 0x19
    assert not any(raw[tail[0][1] + 4:tail[0][1] + tail[0][2]])
    new_section = bytearray(raw[sec:sec + offset]) + packed
    p.put24(new_section, 0, len(new_section))
    new_section += b'\0' * (-len(new_section) % 4)
    padding = fsize - 24 - len(new_section)
    assert padding >= 4
    pad = bytearray(padding)
    p.put24(pad, 0, padding)
    pad[3] = 0x19
    raw[fpos + 24:fpos + fsize] = new_section + pad
    p.fix_ffs(raw, fpos, fsize)
    p.check_ffs(raw, fpos, fsize)
    assert raw[:112] == old_raw[:112]
    for guid, pos, size in outer_files:
        if guid != outer_guid:
            assert raw[pos:pos + size] == old_raw[pos:pos + size]
    new_kernel = gzip.compress(bytes(raw), compresslevel=9, mtime=0) + suffix
    verify = zlib.decompressobj(31)
    assert verify.decompress(new_kernel) + verify.flush() == raw
    assert verify.unused_data == suffix
    header = bytearray(image[:4096])
    struct.pack_into('<I', header, 8, len(new_kernel))
    old_tail = 4096 + p.align(kernel_size, 4096)
    result = header + new_kernel + b'\0' * (-len(new_kernel) % 4096) + image[old_tail:]
    assert result[4096 + p.align(len(new_kernel), 4096):] == image[old_tail:]
    output = args.output.resolve()
    assert output != source, 'Cannot overwrite source image'
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(result)
    output.with_suffix('.json').write_text(json.dumps({
        'source': str(source), 'source_sha256': sha(image),
        'image_sha256': sha(result), 'changed_inner_ffs': changed,
        'displayed_model': 'iPhone 15 Pro Max',
        'change': 'Only identity product-name/product-description text: two equal-length digit substitutions in each of three loader routes',
        'hardware_identity_and_capabilities_unchanged': True,
        'other_inner_ffs_unchanged': True, 'bootshim_unchanged': True,
        'dtb_unchanged': True, 'ramdisk_and_signature_bytes_unchanged': True,
        'compression_roundtrips': 'passed', 'device_validated': False,
    }, indent=2) + '\n')
    print(output)


if __name__ == '__main__':
    main()
