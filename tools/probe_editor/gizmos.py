"""Native viewport gizmo presentation and hit testing; no document mutation."""

import math

from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QPen, QPainter
from PySide6.QtWidgets import QWidget

from .math3d import add, sub, mul, axes, corners, dot

COLORS = ["#ed7770", "#84d57e", "#7fafff"]


class Overlay(QWidget):
    """Qt paints handles above the GL widget, without sharing mutable GL state."""

    def __init__(self, viewport):
        super().__init__(viewport)
        self.viewport = viewport
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        self.setAttribute(Qt.WidgetAttribute.WA_NoSystemBackground)

    def paintEvent(self, event):
        view = self.viewport
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        view.gizmos.draw(
            painter,
            view.camera,
            self.width(),
            self.height(),
            view.volumes,
            view.selection,
            view.mode,
        )
        painter.setPen(QColor("#e1e6ed"))
        painter.drawText(
            14,
            self.height() - 14,
            f"RMB + WASD fly | Q/E down/up | Shift faster | {view.speed:.0f} units/s",
        )


def segment_distance(point, a, b):
    delta = sub(b[:2], a[:2])
    denominator = dot(delta, delta)
    t = (
        max(0, min(1, dot(sub(point, a[:2]), delta) / denominator))
        if denominator
        else 0
    )
    return math.hypot(*sub(point, add(a[:2], mul(delta, t))))


class Gizmos:
    def __init__(self):
        self.handles = []

    def hit(self, mouse):
        candidates = []
        for handle in self.handles:
            distance = (
                segment_distance(mouse, *handle["segment"])
                if "segment" in handle
                else math.hypot(*sub(mouse, handle["point"][:2]))
            )
            if distance < (7 if "segment" in handle else 11):
                candidates.append((distance, handle))
        return min(candidates, key=lambda pair: pair[0])[1] if candidates else None

    def draw(self, painter, camera, width, height, volumes, selected, mode):
        self.handles = []
        project = lambda point: camera.project(point, width, height)

        def line(a, b, color, width=1):
            # Clip against the camera near plane before projecting.
            forward = camera.basis()[0]
            za, zb = (
                dot(sub(a, camera.position), forward),
                dot(sub(b, camera.position), forward),
            )
            if max(za, zb) <= 1.01:
                return None
            if za <= 1.01:
                a = add(a, mul(sub(b, a), (1.02 - za) / (zb - za)))
            if zb <= 1.01:
                b = add(
                    b,
                    mul(
                        sub(a, b),
                        (1.02 - zb) / (dot(sub(a, camera.position), forward) - zb),
                    ),
                )
            a, b = project(a), project(b)
            if a is None or b is None:
                return None
            painter.setPen(QPen(QColor(color), width))
            painter.drawLine(QPointF(*a[:2]), QPointF(*b[:2]))
            return a, b

        def mark(point, color, square=False, radius=5):
            painter.setPen(QPen(QColor("#12191f"), 1))
            painter.setBrush(QColor(color))
            if square:
                painter.drawRect(
                    QRectF(point[0] - radius, point[1] - radius, radius * 2, radius * 2)
                )
            else:
                painter.drawEllipse(QPointF(*point[:2]), radius, radius)

        # Draw every volume outline, then place selected gizmos on top.
        for index, volume in enumerate(volumes):
            chosen = index == selected
            points = corners(volume)
            for i in range(8):
                for axis in range(3):
                    if not i & (1 << axis):
                        segment = line(
                            points[i],
                            points[i | (1 << axis)],
                            "#79ddc6" if chosen else "#69898e",
                            2 if chosen else 1,
                        )
                        if segment and mode == "select":
                            self.handles.append(
                                dict(kind="select", index=index, segment=segment)
                            )
            capture = project(volume["capture"])
            if capture:
                mark(capture, "#8fc7ff", radius=6 if chosen else 4)
                painter.setPen(QColor("#e1f2f0"))
                painter.drawText(
                    QPointF(capture[0] + 10, capture[1] - 10),
                    f"{volume['name']} [{volume['priority']}]",
                )
                if mode == "select":
                    self.handles.append(dict(kind="select", index=index, point=capture))
        if not 0 <= selected < len(volumes) or mode == "select":
            return
        volume = volumes[selected]
        basis = axes(volume["rotationDegrees"])
        origin = volume["capture"] if mode == "capture" else volume["center"]
        center = project(origin)
        if mode == "resize":
            for axis in range(3):
                for sign in (-1, 1):
                    point = project(
                        add(origin, mul(basis[axis], volume["halfSize"][axis] * sign))
                    )
                    if point:
                        mark(point, "#ffd18a", True)
                        self.handles.append(
                            dict(kind="face", axis=axis, sign=sign, point=point)
                        )
            return
        if center is None:
            return
        radius = max(8, center[2] * 90 / max(1, height * math.sqrt(3) / 2))
        if mode == "rotate":
            for axis in range(3):
                a, b = basis[(axis + 1) % 3], basis[(axis + 2) % 3]
                points = [
                    add(
                        origin,
                        add(
                            mul(a, radius * math.cos(i * math.tau / 64)),
                            mul(b, radius * math.sin(i * math.tau / 64)),
                        ),
                    )
                    for i in range(65)
                ]
                for i in range(64):
                    segment = line(points[i], points[i + 1], COLORS[axis], 2)
                    if segment:
                        self.handles.append(
                            dict(kind="rotate", axis=axis, segment=segment)
                        )
            return
        for axis in range(3):
            end = add(origin, mul(basis[axis], radius))
            segment = line(origin, end, COLORS[axis], 3)
            point = project(end)
            if segment and point and math.hypot(*sub(point[:2], center[:2])) > 10:
                mark(point, COLORS[axis], radius=5)
                self.handles.append(dict(kind="axis", axis=axis, segment=segment))
        mark(center, "#9bdfea", True)
        self.handles.append(dict(kind="plane", point=center))
