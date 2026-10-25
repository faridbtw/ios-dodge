#!/usr/bin/env python3
"""Replace the three loader PE sections in a COPY of the exact Dodge RAM boot image.

Keeps inner FFS locations/allocation, other FFS files, BootShim, DTB suffix,
ramdisk and signature bytes. Recomputes changed FFS checksums and validates
LZMA/gzip round trips. Never accesses the phone or overwrites release files.
"""
import argparse
import gzip
import hashlib
import json
import lzma
from pathlib import Path
import struct
import uuid
import zlib

ROOT = Path(__file__).resolve().parents[1]
SOURCE_SHA = 'f26410ca510dcb7352f5c0b814ca435aaefe05bdc3ed1697ef5091b79d588cb8'
TARGETS = {'fb14ecd8-9432-48b8-a692-ae4bc19d568b',
    '50670071-478f-4be7-ad13-8754f379c62f', '643d4788-63c8-40d2-bd7a-6842e66850d1'}
LZMA_GUID = uuid.UUID('ee4e5898-3914-4259-9d6e-dc7bd79403cf').bytes_le


def align(value, step): return (value + step - 1) & ~(step - 1)
def u24(blob, offset): return int.from_bytes(blob[offset:offset + 3], 'little')
def put24(blob, offset, value):
    if not 4 <= value < 0xffffff: raise ValueError('Unsupported section size')
    blob[offset:offset + 3] = value.to_bytes(3, 'little')


def files(blob, base):
    if blob[base + 40:base + 44] != b'_FVH': raise ValueError('Not a firmware volume')
    length = struct.unpack_from('<Q', blob, base + 32)[0]
    header = struct.unpack_from('<H', blob, base + 48)[0]
    if base + length > len(blob) or sum(struct.unpack_from('<' + str(header // 2) + 'H', blob, base)) & 0xffff:
        raise ValueError('Invalid FV bounds/checksum')
    pos = base + align(header, 8)
    while pos + 24 <= base + length:
        if blob[pos:pos + 24] == b'\xff' * 24: break
        size = u24(blob, pos + 20)
        if size < 24 or size == 0xffffff or pos + size > base + length:
            raise ValueError(f'Invalid FFS bounds at {pos:x}')
        yield str(uuid.UUID(bytes_le=bytes(blob[pos:pos + 16]))), pos, size
        pos = align(pos + size, 8)


def sections(blob, start, end):
    pos = start
    while pos + 4 <= end:
        size = u24(blob, pos)
        if size < 4 or size == 0xffffff or pos + size > end:
            raise ValueError(f'Invalid section at {pos:x}')
        yield blob[pos + 3], pos, size
        pos = align(pos + size, 4)


def fix_ffs(blob, pos, size):
    blob[pos + 17] = (-sum(blob[pos + 24:pos + size])) & 0xff if blob[pos + 19] & 0x40 else 0xaa
    header = bytearray(blob[pos:pos + 24])
    header[16] = header[17] = header[23] = 0
    blob[pos + 16] = (-sum(header)) & 0xff


def check_ffs(blob, pos, size):
    header = bytearray(blob[pos:pos + 24]); header[17] = header[23] = 0
    if sum(header) & 0xff: raise ValueError('FFS header checksum failed')
    if blob[pos + 19] & 0x40:
        if (sum(blob[pos + 24:pos + size]) + blob[pos + 17]) & 0xff:
            raise ValueError('FFS payload checksum failed')
    elif blob[pos + 17] != 0xaa: raise ValueError('FFS fixed checksum marker failed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--loader', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    source = (ROOT / 'dodge-known-v31-usb-loader.img').read_bytes()
    if hashlib.sha256(source).hexdigest() != SOURCE_SHA: raise ValueError('Release image hash mismatch')
    output = args.output.resolve()
    if output == (ROOT / 'dodge-known-v31-usb-loader.img').resolve(): raise ValueError('Cannot overwrite release')
    loader = args.loader.read_bytes()
    pe = struct.unpack_from('<I', loader, 0x3c)[0]
    if loader[:2] != b'MZ' or loader[pe:pe + 4] != b'PE\0\0' or struct.unpack_from('<H', loader, pe + 4)[0] != 0xaa64:
        raise ValueError('Expected ARM64 PE loader')
    if struct.unpack_from('<Q', loader, pe + 48)[0] != 0xc4000000:
        raise ValueError('Expected loader preferred base 0xC4000000')
    if source[:8] != b'ANDROID!' or struct.unpack_from('<I', source, 40)[0] != 4:
        raise ValueError('Expected Android boot image v4')
    kernel_size, ramdisk_size = struct.unpack_from('<II', source, 8)
    kernel = source[4096:4096 + kernel_size]
    decoder = zlib.decompressobj(31)
    raw = bytearray(decoder.decompress(kernel) + decoder.flush())
    if not decoder.eof: raise ValueError('Incomplete gzip stream')
    suffix = decoder.unused_data
    outer_base = 112
    outer_files = list(files(raw, outer_base))
    matches = []
    for guid, pos, size in outer_files:
        for kind, sec, length in sections(raw, pos + 24, pos + size):
            if kind == 2 and raw[sec + 4:sec + 20] == LZMA_GUID:
                matches.append((guid, pos, size, sec, length))
    if len(matches) != 1: raise ValueError('Expected one LZMA DXE section')
    outer_guid, fpos, fsize, sec, slen = matches[0]
    data_offset = struct.unpack_from('<H', raw, sec + 20)[0]
    if data_offset != 24 or sec != fpos + 24: raise ValueError('Unexpected guided-section layout')
    compressed = bytes(raw[sec + data_offset:sec + slen])
    inner_original = lzma.decompress(compressed, format=lzma.FORMAT_ALONE)
    inner = bytearray(inner_original)
    inner_base = 8
    original_loader = (ROOT / 'loader-known-v31.efi').read_bytes()
    changed, original_files = [], list(files(inner, inner_base))
    for guid, pos, size in original_files:
        check_ffs(inner, pos, size)
        if guid not in TARGETS: continue
        pe_sections = [(s, n) for kind, s, n in sections(inner, pos + 24, pos + size) if kind == 16]
        if len(pe_sections) != 1: raise ValueError('Expected one PE32 section per loader route')
        start, length = pe_sections[0]; start += 4; length -= 4
        if inner[start:start + len(original_loader)] != original_loader:
            raise ValueError('Loader route content does not match release')
        if any(inner[start + len(original_loader):start + length]): raise ValueError('Nonzero PE padding')
        if len(loader) > length: raise ValueError('Loader exceeds fixed PE allocation')
        inner[start:start + length] = loader + b'\0' * (length - len(loader))
        fix_ffs(inner, pos, size);check_ffs(inner, pos, size)
        changed.append(guid)
    if set(changed) != TARGETS: raise ValueError('Missing loader routes')
    for guid, pos, size in original_files:
        if guid not in TARGETS and inner[pos:pos + size] != inner_original[pos:pos + size]:
            raise ValueError('An unrelated inner FFS changed')
    prop = compressed[0]; lc, remainder = prop % 9, prop // 9
    filters = [{'id': lzma.FILTER_LZMA1, 'dict_size': int.from_bytes(compressed[1:5], 'little'),
        'lc': lc, 'lp': remainder % 5, 'pb': remainder // 5}]
    packed = bytearray(lzma.compress(bytes(inner), format=lzma.FORMAT_ALONE, filters=filters))
    packed[5:13] = len(inner).to_bytes(8, 'little')
    if lzma.decompress(packed, format=lzma.FORMAT_ALONE) != inner: raise ValueError('LZMA round trip failed')
    # The remaining outer FFS section is padding, not another executable.
    tail = list(sections(raw, align(sec + slen, 4), fpos + fsize))
    if len(tail) != 1 or tail[0][0] != 0x19 or any(raw[tail[0][1] + 4:tail[0][1] + tail[0][2]]):
        raise ValueError('Outer padding section is not empty')
    guided = bytearray(raw[sec:sec + data_offset]) + packed
    put24(guided, 0, len(guided))
    guided += b'\0' * (-len(guided) % 4)
    padding = fsize - 24 - len(guided)
    if padding < 4: raise ValueError('Compressed loader exceeds outer FFS allocation')
    pad_section = bytearray(padding);put24(pad_section, 0, padding);pad_section[3] = 0x19
    raw[fpos + 24:fpos + fsize] = guided + pad_section
    fix_ffs(raw, fpos, fsize);check_ffs(raw, fpos, fsize)
    original_raw = zlib.decompress(kernel, 31)
    for guid, pos, size in outer_files:
        if guid != outer_guid and raw[pos:pos + size] != original_raw[pos:pos + size]:
            raise ValueError('An unrelated outer FFS changed')
    if raw[:outer_base] != original_raw[:outer_base]: raise ValueError('BootShim changed')
    new_kernel = gzip.compress(bytes(raw), compresslevel=9, mtime=0) + suffix
    verify = zlib.decompressobj(31)
    if verify.decompress(new_kernel) + verify.flush() != raw or verify.unused_data != suffix:
        raise ValueError('gzip/DTB round trip failed')
    header = bytearray(source[:4096]);struct.pack_into('<I', header, 8, len(new_kernel))
    old_tail = 4096 + align(kernel_size, 4096)
    new_image = header + new_kernel + b'\0' * (-len(new_kernel) % 4096) + source[old_tail:]
    if source[old_tail:old_tail + ramdisk_size] != new_image[4096 + align(len(new_kernel), 4096):][:ramdisk_size]:
        raise ValueError('Ramdisk changed')
    output.parent.mkdir(parents=True, exist_ok=True);output.write_bytes(new_image)
    output.with_suffix('.json').write_text(json.dumps({'source_sha256': SOURCE_SHA,
        'image_sha256':hashlib.sha256(new_image).hexdigest(),'loader_sha256':hashlib.sha256(loader).hexdigest(),
        'changed_inner_ffs':changed,'other_ffs_unchanged':True,'bootshim_unchanged':True,
        'dtb_unchanged':True,'ramdisk_and_signature_bytes_unchanged':True,
        'compression_roundtrips':'passed','device_validated':False},indent=2)+'\n')
    print(output)


if __name__ == '__main__': main()
