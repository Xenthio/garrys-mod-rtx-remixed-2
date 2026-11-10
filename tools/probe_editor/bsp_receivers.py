"""Read compiled face/material assignments without inferring cubemap influence."""

import struct


def material_receivers(bsp):
    """Group Source dface_t records by their actual material name.

    These indices belong to this fingerprinted BSP, not Hammer side IDs. Include
    brush-model and displacement base faces; this is an assignment inventory,
    not a claim that every face is a visible reflection receiver.
    """
    faces, infos, data = bsp.lump(7), bsp.lump(6), bsp.lump(2)
    strings, table = bsp.lump(43), bsp.lump(44)
    for name, payload, stride in (('faces', faces, 56), ('texinfo', infos, 72),
                                  ('texdata', data, 32), ('string table', table, 4)):
        if len(payload) % stride:
            raise ValueError(f'Unsupported compiled receiver {name} layout')
    groups = {}
    names = {}
    for face in range(len(faces) // 56):
        info = struct.unpack_from('<h', faces, face * 56 + 10)[0]
        if info == -1:  # Source permits faces without texture information.
            continue
        if info < 0 or info >= len(infos) // 72:
            raise ValueError(f'Invalid texinfo on compiled face {face}')
        texture = struct.unpack_from('<i', infos, info * 72 + 68)[0]
        if texture < 0 or texture >= len(data) // 32:
            raise ValueError(f'Invalid texdata on compiled face {face}')
        if texture not in names:
            name_id = struct.unpack_from('<i', data, texture * 32 + 12)[0]
            if name_id < 0 or name_id >= len(table) // 4:
                raise ValueError('Invalid compiled receiver material string index')
            offset = struct.unpack_from('<i', table, name_id * 4)[0]
            end = strings.find(b'\0', offset) if 0 <= offset < len(strings) else -1
            if end < 0 or end - offset > 65536:
                raise ValueError('Invalid compiled receiver material string')
            name = strings[offset:end].decode('utf-8', errors='strict')
            if not name:
                raise ValueError('Empty compiled receiver material name')
            names[texture] = name
        groups.setdefault(names[texture], []).append(face)
    return [dict(material=name, compiledFaceIndices=indices)
            for name, indices in sorted(groups.items())]
