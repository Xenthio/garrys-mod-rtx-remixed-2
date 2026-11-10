"""Map-specific cubemap authoring data, independent of the viewer and renderer."""
import math
import uuid
import hashlib
import json

MAX_RUNTIME_VOLUMES = 64


def vector(value, name, positive=False):
    if not isinstance(value, list) or len(value) != 3:
        raise ValueError(f'{name} must contain three numbers')
    if any(isinstance(v, bool) or not isinstance(v, (float, int)) or not math.isfinite(v)
           or abs(v) > 1e8 or (positive and v <= 0) for v in value):
        raise ValueError(f'Invalid {name}')
    return list(value)


def validate(document, map_hash):
    if not isinstance(document, dict) or document.get('version') != 1 or document.get('units') != 'bsp':
        raise ValueError('Expected version 1 cubemap bounds in BSP units')
    if document.get('mapSha256') != map_hash:
        raise ValueError('These bounds belong to a different BSP revision')
    volumes = document.get('volumes')
    if not isinstance(volumes, list) or len(volumes) > 4096:
        raise ValueError('Expected at most 4096 cubemap volumes')
    result, ids = [], set()
    for volume in volumes:
        if not isinstance(volume, dict):
            raise ValueError('Invalid cubemap volume')
        identity = volume.get('id')
        if not isinstance(identity, str) or len(identity) > 80 or not identity or identity in ids:
            raise ValueError('Cubemap IDs must be unique nonempty strings')
        ids.add(identity)
        priority = volume.get('priority')
        if isinstance(priority, bool) or not isinstance(priority, int) or not -32768 <= priority <= 32767:
            raise ValueError('Priority must be an integer from -32768 to 32767')
        name = volume.get('name', '')
        if not isinstance(name, str) or len(name) > 200:
            raise ValueError('Invalid cubemap name')
        result.append(dict(id=identity, name=name, priority=priority,
                           center=vector(volume.get('center'), 'center'),
                           halfSize=vector(volume.get('halfSize'), 'halfSize', True),
                           rotationDegrees=vector(volume.get('rotationDegrees'), 'rotationDegrees'),
                           capture=vector(volume.get('capture'), 'capture')))
        if 'sampling' in volume:
            sampling = volume['sampling']
            if not isinstance(sampling, dict):
                raise ValueError('Invalid reflection sampling settings')
            lo = vector(sampling.get('minimum'), 'influence minimum')
            hi = vector(sampling.get('maximum'), 'influence maximum')
            distance = sampling.get('blendDistance')
            if any(a >= b for a, b in zip(lo, hi)) or isinstance(distance, bool) or \
                    not isinstance(distance, (int, float)) or not math.isfinite(distance) or not 0 <= distance <= 1e8:
                raise ValueError('Invalid reflection influence bounds or blend distance')
            result[-1]['sampling'] = dict(minimum=lo, maximum=hi, blendDistance=distance)
    clean = dict(version=1, units='bsp', mapSha256=map_hash,
                 mapName=str(document.get('mapName', ''))[:200], volumes=result)
    if 'reference' in document:
        reference = document['reference']
        if not isinstance(reference, dict) or not isinstance(reference.get('activeHash'), str) or \
                not reference['activeHash'].isdigit() or len(reference['activeHash']) > 20:
            raise ValueError('Invalid reference capture identity')
        digest = reference.get('sha256')
        if not isinstance(digest, str) or len(digest) != 64 or any(c not in '0123456789abcdef' for c in digest):
            raise ValueError('Invalid reference report fingerprint')
        clean['reference'] = dict(activeHash=reference['activeHash'], sha256=digest)
    return clean


def empty(map_name, map_hash):
    return dict(version=1, units='bsp', mapName=map_name, mapSha256=map_hash, volumes=[])


def new_volume(position):
    return dict(id=str(uuid.uuid4()), name='Cubemap', priority=0, center=list(position),
                halfSize=[128, 128, 128], rotationDegrees=[0, 0, 0], capture=list(position))


def projection_transform(probe, lo, hi):
    """Convert a native rigid projection frame to the editor's Rz*Ry*Rx box."""
    rows = probe.get('worldToLocal', [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0]])
    if not isinstance(rows, list) or len(rows) != 3 or any(
            not isinstance(row, list) or len(row) != 4 for row in rows):
        raise ValueError('Invalid projection frame')
    if any(isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v)
           for row in rows for v in row):
        raise ValueError('Invalid projection frame')
    for a in range(3):
        for b in range(3):
            if abs(sum(rows[a][c]*rows[b][c] for c in range(3)) - (a == b)) > 1e-4:
                raise ValueError('Projection frame must be rigid')
    rotation = [[rows[c][r] for c in range(3)] for r in range(3)]
    determinant = sum(rotation[0][c] * (rotation[1][(c+1)%3]*rotation[2][(c+2)%3] -
                       rotation[1][(c+2)%3]*rotation[2][(c+1)%3]) for c in range(3))
    if determinant < .999:
        raise ValueError('Projection frame must preserve orientation')
    local = [(a+b)/2 - rows[c][3] for c,(a,b) in enumerate(zip(lo,hi))]
    center = [sum(rotation[r][c]*local[c] for c in range(3)) for r in range(3)]
    pitch = math.asin(max(-1, min(1, -rotation[2][0])))
    if abs(math.cos(pitch)) > 1e-6:
        roll = math.atan2(rotation[2][1], rotation[2][2])
        yaw = math.atan2(rotation[1][0], rotation[0][0])
    else:
        roll = 0
        yaw = math.atan2(-rotation[0][1], rotation[1][1])
    return center, [math.degrees(v) for v in (roll, pitch, yaw)]


def from_probe_report(payload, map_name, map_hash):
    """Import an explicitly selected native report; it has no BSP fingerprint.

    Native report coordinates are host units already. metersPerUnit is metadata,
    not a multiplier to apply again. Stable source IDs and report provenance
    support subsequent fit comparisons.
    """
    report = json.loads(payload)
    if not isinstance(report, dict) or report.get('version') != 1 or \
            str(report.get('activeHash', '0')) in ('', '0'):
        raise ValueError('Expected a published version 1 probe report')
    scale = report.get('metersPerUnit')
    if isinstance(scale, bool) or not isinstance(scale, (int, float)) or not math.isfinite(scale) or scale <= 0:
        raise ValueError('Invalid probe report scene scale')
    probes = report.get('probes')
    if not isinstance(probes, list) or len(probes) > 4096:
        raise ValueError('Invalid probe report size')
    result = empty(map_name, map_hash)
    result['reference'] = dict(activeHash=str(report['activeHash']), sha256=hashlib.sha256(payload).hexdigest())
    for index, probe in enumerate(probes):
        if not isinstance(probe, dict):
            raise ValueError('Invalid probe record')
        lo, hi = vector(probe.get('minimum'), 'minimum'), vector(probe.get('maximum'), 'maximum')
        if any(a >= b for a, b in zip(lo, hi)):
            raise ValueError('Invalid projection bounds in probe report')
        center, rotation = projection_transform(probe, lo, hi)
        volume = dict(id=f'auto-{index}-{report["activeHash"]}', name=f'Automatic {index}',
                      priority=probe.get('priority', 0), center=center, halfSize=[(b-a)/2 for a,b in zip(lo,hi)],
                      rotationDegrees=rotation, capture=vector(probe.get('position'), 'position'))
        if 'influenceMinimum' in probe or 'influenceMaximum' in probe or 'featherDistance' in probe:
            influence_lo = vector(probe.get('influenceMinimum'), 'influence minimum')
            influence_hi = vector(probe.get('influenceMaximum'), 'influence maximum')
            feather = vector(probe.get('featherDistance'), 'feather distance', True)
            half = [(b-a)/2 for a, b in zip(influence_lo, influence_hi)]
            if any(h <= 0 for h in half):
                raise ValueError('Invalid influence bounds in probe report')
            close = lambda a, b: math.isclose(a, b, rel_tol=1e-5, abs_tol=1e-5)
            if all(close(width, h*.2) for width, h in zip(feather, half)):
                distance = 0
            else:
                distance = max(feather)
                if not all(close(width, min(distance, h)) for width, h in zip(feather, half)):
                    raise ValueError('Probe report uses unsupported independent axis feather distances')
            # The editor recentres the projection frame around its box midpoint.
            # Rebase influence into that same frame without rescaling host units.
            midpoint = [(a+b)/2 for a, b in zip(lo, hi)]
            legacy = influence_lo == lo and influence_hi == hi and close(distance, .5 / scale)
            if not legacy:
                volume['sampling'] = dict(minimum=[a-c for a, c in zip(influence_lo, midpoint)],
                    maximum=[b-c for b, c in zip(influence_hi, midpoint)], blendDistance=distance)
        result['volumes'].append(volume)
    return validate(result, map_hash)


def runtime_binary(value):
    """Bounded adapter sidecar; JSON remains the editable source of truth."""
    import struct
    clean = validate(value, value.get('mapSha256'))
    digest = clean['mapSha256']
    if not isinstance(digest, str) or len(digest) != 64 or any(c not in '0123456789abcdef' for c in digest):
        raise ValueError('Runtime export requires a BSP SHA-256 fingerprint')
    if len(clean['volumes']) > MAX_RUNTIME_VOLUMES:
        raise ValueError(f'The renderer currently supports at most {MAX_RUNTIME_VOLUMES} authored cubemaps')
    extended = any('sampling' in volume for volume in clean['volumes'])
    result = bytearray((b'BSPCUBE2' if extended else b'BSPCUBE1') + bytes.fromhex(digest) +
                       struct.pack('<I', len(clean['volumes'])))
    identities = set()
    for volume in clean['volumes']:
        identity = int.from_bytes(hashlib.sha256(volume['id'].encode()).digest()[:8], 'little') or 1
        if identity in identities:
            raise ValueError('Cubemap runtime ID collision')
        identities.add(identity)
        x,y,z = [math.radians(v) for v in volume['rotationDegrees']]
        cx,sx,cy,sy,cz,sz = math.cos(x),math.sin(x),math.cos(y),math.sin(y),math.cos(z),math.sin(z)
        rotation = [[cz*cy, cz*sy*sx-sz*cx, cz*sy*cx+sz*sx],
                    [sz*cy, sz*sy*sx+cz*cx, sz*sy*cx-cz*sx], [-sy, cy*sx, cy*cx]]
        rows = [[rotation[c][r] for c in range(3)] for r in range(3)]
        for row in rows:
            row.append(-sum(a*b for a,b in zip(row, volume['center'])))
        local = [sum(row[c]*volume['capture'][c] for c in range(3))+row[3] for row in rows]
        if any(abs(v) > h for v,h in zip(local, volume['halfSize'])):
            raise ValueError('A runtime capture point must lie inside its projection volume')
        result.extend(struct.pack('<Q6f12f3fi', identity, *[-h for h in volume['halfSize']],
            *volume['halfSize'], *[v for row in rows for v in row], *volume['capture'], volume['priority']))
        if extended:
            # Documents use Source BSP units. Preserve old half-metre feathering
            # for unchanged volumes in a mixed old/new document.
            sampling = volume.get('sampling', dict(minimum=[-h for h in volume['halfSize']],
                maximum=volume['halfSize'], blendDistance=.5 / .0254))
            result.extend(struct.pack('<7f', *sampling['minimum'], *sampling['maximum'], sampling['blendDistance']))
    return bytes(result)
