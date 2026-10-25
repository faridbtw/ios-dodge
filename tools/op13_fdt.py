"""Read a captured flattened device tree without host/device mutations."""
import struct


def read_fdt(data):
    if len(data) < 40:
        raise ValueError('Short FDT header')
    magic, total, structure, strings, _, version, _, _, strings_size, structure_size = struct.unpack_from('>10I', data)
    if magic != 0xd00dfeed or total > len(data) or version < 17:
        raise ValueError('Unsupported FDT header')
    if structure + structure_size > total or strings + strings_size > total:
        raise ValueError('FDT blocks out of bounds')
    names = data[strings:strings + strings_size]
    end = structure + structure_size
    at, stack, nodes = structure, [], {}
    while at + 4 <= end:
        tag = struct.unpack_from('>I', data, at)[0]
        at += 4
        if tag == 1:
            stop = data.index(0, at, end)
            stack.append(data[at:stop].decode())
            at = (stop + 4) & ~3
            path = '/'.join(stack) or '/'
            if path in nodes:
                raise ValueError('Duplicate FDT node')
            nodes[path] = {}
        elif tag == 2:
            if not stack:
                raise ValueError('Unbalanced FDT node')
            stack.pop()
        elif tag == 3:
            if not stack or at + 8 > end:
                raise ValueError('Invalid FDT property')
            size, name_offset = struct.unpack_from('>II', data, at)
            at += 8
            if at + size > end or name_offset >= len(names):
                raise ValueError('FDT property out of bounds')
            stop = names.index(0, name_offset)
            name = names[name_offset:stop].decode()
            props = nodes['/'.join(stack) or '/']
            if name in props:
                raise ValueError('Duplicate FDT property')
            props[name] = data[at:at + size]
            at = (at + size + 3) & ~3
        elif tag == 9:
            if stack:
                raise ValueError('Unclosed FDT node')
            return nodes
        elif tag != 4:
            raise ValueError('Unknown FDT token')
    raise ValueError('Missing FDT end marker')


def cells(data):
    if len(data) % 4:
        raise ValueError('Unaligned FDT cells')
    return list(struct.unpack('>' + str(len(data) // 4) + 'I', data))
