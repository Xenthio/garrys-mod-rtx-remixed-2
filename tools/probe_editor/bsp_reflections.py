"""Extract authored BSP parallax projection references without inventing influence bounds."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re

MAX_ENTITIES = 262144
MAX_ENTITY_BYTES = 16 << 20
MAX_TOKEN = 65536
MAX_VOLUMES = 4096
TOKEN = re.compile(r'\s+|//[^\r\n]*|[{}]|"(?:\\.|[^"\\])*"')


def entities(payload):
    if len(payload) > MAX_ENTITY_BYTES:
        raise ValueError('Entity lump exceeds reference budget')
    text = payload.rstrip(b'\0').decode('utf-8', errors='replace')
    if '\0' in text:
        raise ValueError('Embedded null in entity lump')
    current, key, cursor, count = None, None, 0, 0
    while cursor < len(text):
        token = TOKEN.match(text, cursor)
        if token is None:
            raise ValueError(f'Malformed entity text at character {cursor}')
        value = token.group()
        cursor = token.end()
        if value.isspace() or value.startswith('//'):
            continue
        if len(value) > MAX_TOKEN:
            raise ValueError('Entity token exceeds reference budget')
        if value == '{':
            if current is not None:
                raise ValueError('Nested entity block')
            current = []
        elif value == '}':
            if current is None or key is not None:
                raise ValueError('Incomplete entity block')
            count += 1
            if count > MAX_ENTITIES:
                raise ValueError('Entity count exceeds reference budget')
            yield current
            current = None
        else:
            if current is None:
                raise ValueError('Entity key outside a block')
            value = re.sub(r'\\(["\\])', r'\1', value[1:-1])
            if key is None:
                key = value.lower()
            else:
                current.append((key, value))
                key = None
                if len(current) > 4096:
                    raise ValueError('Entity field count exceeds reference budget')
    if current is not None or key is not None:
        raise ValueError('Unterminated entity block')


def vector(value, name):
    if value is None:
        raise ValueError(f'Missing cubemap {name}')
    try:
        result = [float(v) for v in value.split()]
    except ValueError as error:
        raise ValueError(f'Invalid cubemap {name}') from error
    if len(result) != 3 or any(not math.isfinite(v) or abs(v) > 1e8 for v in result):
        raise ValueError(f'Invalid cubemap {name}')
    return result


def reference(payload, map_name, map_hash):
    if not re.fullmatch('[0-9a-f]{64}', map_hash):
        raise ValueError('Reference requires a BSP SHA-256 fingerprint')
    volumes, seeds = [], 0
    for index, fields in enumerate(entities(payload)):
        values = dict(fields)
        classes = [value.lower() for key, value in fields if key == 'classname']
        if 'env_cubemap' not in classes:
            continue
        if len(classes) != 1:
            raise ValueError(f'Ambiguous cubemap classname in entity {index}')
        seeds += 1
        if not any(key == 'parallax' and value.strip() == '1' for key, value in fields):
            continue
        for name in ('origin', 'parallax', 'boxmin', 'boxmax', 'angles', 'sides', 'hammerid'):
            if sum(key == name for key, _ in fields) > 1:
                raise ValueError(f'Ambiguous cubemap field {name} in entity {index}')
        capture = vector(values.get('origin'), 'origin')
        lo, hi = vector(values.get('boxmin'), 'boxmin'), vector(values.get('boxmax'), 'boxmax')
        if any(a >= b for a, b in zip(lo, hi)):
            raise ValueError(f'Invalid projection extent in entity {index}')
        sides = values.get('sides', '').split()
        if any(not re.fullmatch('[0-9]{1,10}', side) or int(side) > 0xffffffff for side in sides):
            raise ValueError(f'Invalid Hammer side assignment in entity {index}')
        angles = vector(values.get('angles', '0 0 0'), 'angles')
        if len(volumes) >= MAX_VOLUMES:
            raise ValueError('Projection reference volume count exceeds budget')
        volumes.append(dict(entityIndex=index, hammerId=values.get('hammerid'), capture=capture,
            projectionMinimumOffset=lo, projectionMaximumOffset=hi, sourceAngles=angles,
            captureInside=all(a <= 0 <= b for a, b in zip(lo, hi)), assignedHammerSides=sides))
    return dict(format='bsp-parallax-projection-reference-v1', units='bsp', mapName=map_name,
        mapSha256=map_hash, cubemapEntityCount=seeds, volumes=volumes,
        interpretation='Raw authored projection offsets. Coordinate/rotation conventions require runtime verification. '
            'Influence bounds and overlap priorities are unspecified; Hammer side IDs are not compiled face indices.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bsp', type=Path)
    parser.add_argument('--output', type=Path, help='Reference JSON destination; defaults to standard output')
    parser.add_argument('--compiled-receivers', action='store_true',
                        help='Include compiled face/material assignments (not Hammer side IDs)')
    parser.add_argument('--capture-assignments', action='store_true',
                        help='Include explicit packed material capture links and compiled receivers')
    args = parser.parse_args()
    from probe_bounds import Bsp
    from .storage import atomic_write
    bsp = Bsp(args.bsp)
    result = reference(bsp.lump(0), args.bsp.stem, hashlib.sha256(bsp.data).hexdigest())
    if args.compiled_receivers or args.capture_assignments:
        from .bsp_receivers import material_receivers
        result['compiledMaterialReceivers'] = material_receivers(bsp)
    if args.capture_assignments:
        from .bsp_captures import capture_assignments
        result['compiledCaptureAssignments'] = capture_assignments(
            bsp, result['compiledMaterialReceivers'], args.bsp.stem, result['volumes'])
    payload = (json.dumps(result, indent=2, allow_nan=False) + '\n').encode('utf-8')
    if args.output:
        atomic_write(args.output, payload)
    else:
        print(payload.decode('utf-8'), end='')


if __name__ == '__main__':
    main()
