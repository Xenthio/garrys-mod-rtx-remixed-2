"""Inventory explicit compiled cubemap assignments, without resolving VMT inheritance."""

import io
import re
import zipfile


MAX_PATCH_BYTES = 1 << 20
MAX_TOTAL_BYTES = 64 << 20
MAX_ARCHIVE_ENTRIES = 262144
QUOTED_PATCH = re.compile(
    r'\s*"patch"\s*\{\s*"include"\s*"([^"\r\n]+)"\s*'
    r'"replace"\s*\{\s*((?:"[^"\r\n]+"\s*"[^"\r\n]*"\s*)+)\}\s*\}\s*', re.I)
SCALAR_PAIR = re.compile(r'"([^"\r\n]+)"\s*"([^"\r\n]*)"')


def canonical_path(value):
    value = value.replace('\\', '/').lower()
    parts = value.split('/')
    if value.startswith('/') or ':' in value or any(p in ('.', '..') for p in parts):
        raise ValueError('Unsafe packed material path')
    return '/'.join(p for p in parts if p)


def capture_assignments(bsp, receivers, map_name, volumes):
    """Recognize only the compiler's simple include/replace-$envmap patch form.

    Arbitrary VMTs, inheritance, conditions and proxies require the real material
    resolver. Report them as unsupported, never guess from the material filename.
    A capture origin match identifies reference entities, not influence bounds.
    """
    payload = bsp.lump(40)
    if not payload:
        return [dict(material=r['material'], status='no-packed-material') for r in receivers]
    origins = {}
    for volume in volumes:
        origins.setdefault(tuple(volume['capture']), []).append(volume['entityIndex'])
    capture_name = re.compile(r'maps/' + re.escape(map_name.lower()) + r'/c(-?\d+)_(-?\d+)_(-?\d+)')
    records = []
    read_bytes = 0
    with zipfile.ZipFile(io.BytesIO(payload)) as archive:
        entries = archive.infolist()
        if len(entries) > MAX_ARCHIVE_ENTRIES:
            raise ValueError('Packed material archive exceeds entry budget')
        materials = {}
        for entry in entries:
            if not entry.filename.lower().endswith('.vmt'):
                continue
            name = canonical_path(entry.filename)
            if name in materials:
                raise ValueError('Ambiguous packed material path: ' + name)
            materials[name] = entry
        for receiver in receivers:
            record = dict(material=receiver['material'], status='no-packed-material')
            records.append(record)
            entry = materials.get('materials/' + canonical_path(receiver['material']) + '.vmt')
            if entry is None:
                continue
            if entry.file_size > MAX_PATCH_BYTES or read_bytes + entry.file_size > MAX_TOTAL_BYTES:
                raise ValueError('Packed material reference exceeds read budget')
            read_bytes += entry.file_size
            text = archive.read(entry).decode('utf-8', errors='strict')
            patch = QUOTED_PATCH.fullmatch(text)
            record['status'] = 'unsupported-material-form'
            if patch is None:
                continue
            pairs = [(key.lower(), value) for key, value in SCALAR_PAIR.findall(patch[2])]
            if len({key for key, _ in pairs}) != len(pairs):
                continue
            replacements = dict(pairs)
            if '$envmap' not in replacements:
                continue
            record['includedMaterial'] = patch[1]
            texture = canonical_path(replacements['$envmap'])
            record['captureTexture'] = texture
            match = capture_name.fullmatch(texture)
            record['status'] = 'nonlocal-capture-texture'
            if match is None:
                continue
            capture = [int(v) for v in match.groups()]
            if any(abs(v) > 1e8 for v in capture):
                raise ValueError('Compiled capture origin exceeds reference bounds')
            record['capture'] = capture
            record['projectionEntityIndices'] = origins.get(tuple(capture), [])
            count = len(record['projectionEntityIndices'])
            record['status'] = ('authored-projection' if count == 1 else
                                'ambiguous-projection' if count else 'capture-without-authored-projection')
    return records
