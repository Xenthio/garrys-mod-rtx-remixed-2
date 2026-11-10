"""BSP-space camera and gizmo geometry, independent of Qt and OpenGL."""

import math


def add(a, b):
    return [x + y for x, y in zip(a, b)]


def sub(a, b):
    return [x - y for x, y in zip(a, b)]


def mul(a, scalar):
    return [x * scalar for x in a]


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def cross(a, b):
    return [
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    ]


def unit(a):
    return mul(a, 1 / max(math.hypot(*a), 1e-20))


def axes(degrees):
    x, y, z = map(math.radians, degrees)
    cx, sx, cy, sy, cz, sz = (
        math.cos(x),
        math.sin(x),
        math.cos(y),
        math.sin(y),
        math.cos(z),
        math.sin(z),
    )
    return [
        [cz * cy, sz * cy, -sy],
        [cz * sy * sx - sz * cx, sz * sy * sx + cz * cx, cy * sx],
        [cz * sy * cx + sz * sx, sz * sy * cx - cz * sx, cy * cx],
    ]


def rotate(degrees, axis, angle):
    """Compose a world-space axis rotation, returning the document's Rz Ry Rx Euler angles."""
    c, s = math.cos(angle), math.sin(angle)
    columns = [
        add(add(mul(p, c), mul(cross(axis, p), s)), mul(axis, dot(axis, p) * (1 - c)))
        for p in axes(degrees)
    ]
    y = math.asin(max(-1, min(1, -columns[0][2])))
    singular = abs(math.cos(y)) < 1e-7
    x = (
        math.atan2(-columns[2][1], columns[1][1])
        if singular
        else math.atan2(columns[1][2], columns[2][2])
    )
    z = 0 if singular else math.atan2(columns[0][1], columns[0][0])
    return list(map(math.degrees, [x, y, z]))


def corners(volume):
    basis = axes(volume["rotationDegrees"])
    return [
        add(
            volume["center"],
            [
                sum(
                    basis[j][k] * volume["halfSize"][j] * (1 if i & (1 << j) else -1)
                    for j in range(3)
                )
                for k in range(3)
            ],
        )
        for i in range(8)
    ]


class Camera:
    def __init__(self):
        self.position = [0, 0, 128]
        self.yaw, self.pitch = math.pi / 4, -0.3

    def basis(self):
        c, s, p, q = (
            math.cos(self.yaw),
            math.sin(self.yaw),
            math.cos(self.pitch),
            math.sin(self.pitch),
        )
        return [c * p, s * p, q], [s, -c, 0], [-c * q, -s * q, p]

    def project(self, point, width, height):
        forward, right, up = self.basis()
        delta = sub(point, self.position)
        z = dot(delta, forward)
        if z <= 1:
            return None
        focal = height * math.sqrt(3) / 2
        return [
            width / 2 + dot(delta, right) * focal / z,
            height / 2 - dot(delta, up) * focal / z,
            z,
        ]

    def ray(self, mouse, width, height):
        forward, right, up = self.basis()
        focal = max(1, height) * math.sqrt(3) / 2
        return unit(
            add(
                forward,
                add(
                    mul(right, (mouse[0] - width / 2) / focal),
                    mul(up, (height / 2 - mouse[1]) / focal),
                ),
            )
        )

    def frame(self, points, width, height):
        if not points:
            return
        lo = [min(p[i] for p in points) for i in range(3)]
        hi = [max(p[i] for p in points) for i in range(3)]
        center = mul(add(lo, hi), 0.5)
        radius = max(64, math.hypot(*sub(hi, lo)) * 0.5)
        angle = min(
            math.pi / 6,
            math.atan(math.tan(math.pi / 6) * max(0.001, width / max(1, height))),
        )
        self.position = sub(
            center, mul(self.basis()[0], radius * 1.1 / math.sin(angle))
        )


def plane_point(origin, direction, center, normal):
    denominator = dot(direction, normal)
    if abs(denominator) < 1e-5:
        return None
    distance = dot(sub(center, origin), normal) / denominator
    return add(origin, mul(direction, distance)) if distance > 0 else None


def axis_distance(origin, direction, center, axis):
    cosine = dot(direction, axis)
    denominator = 1 - cosine * cosine
    if denominator < 0.002:
        return None
    delta = sub(origin, center)
    return (dot(delta, axis) - cosine * dot(delta, direction)) / denominator


def resize_face(original, axis, sign, distance):
    half = list(original["halfSize"])
    half[axis] = max(0.5, half[axis] + sign * distance / 2)
    center = add(
        original["center"],
        mul(
            axes(original["rotationDegrees"])[axis],
            sign * (half[axis] - original["halfSize"][axis]),
        ),
    )
    return dict(halfSize=half, center=center)
