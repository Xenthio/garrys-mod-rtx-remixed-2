"""Native OpenGL viewport: textured worklight preview, fly camera and gizmo drags."""

from array import array
import copy
import ctypes
import math
import time

from OpenGL import GL as gl
from OpenGL.GL.shaders import compileProgram, compileShader
from PySide6.QtCore import Qt, QTimer, Signal
from PySide6.QtGui import QCursor
from PySide6.QtOpenGLWidgets import QOpenGLWidget
from PIL import Image

from .gizmos import Gizmos, Overlay
from .math3d import (
    Camera,
    add,
    sub,
    mul,
    dot,
    cross,
    unit,
    axes,
    axis_distance,
    plane_point,
    resize_face,
    rotate,
)

VERTEX = """#version 330 core
layout(location=0) in vec3 position;
layout(location=1) in vec3 normal;
layout(location=2) in vec2 uv;
uniform vec3 eye, forward, right, up;
uniform float aspect;
out vec3 worldNormal, worldPosition;
out vec2 texcoord;
void main() {
  vec3 d = position-eye;
  float z = dot(d,forward);
  gl_Position = vec4(dot(d,right)*1.7320508/aspect,dot(d,up)*1.7320508,1.00002*z-2.00002,z);
  worldNormal = normal; worldPosition = position; texcoord = uv;
}
"""
FRAGMENT = """#version 330 core
in vec3 worldNormal, worldPosition;
in vec2 texcoord;
uniform sampler2D baseTexture;
uniform vec3 eye;
uniform int textured, worklight;
out vec4 color;
void main() {
  vec3 albedo = textured != 0 ? texture(baseTexture,texcoord).rgb : vec3(.5,.54,.57);
  // A camera worklight plus ambient fill keeps unbaked rooms readable.
  // Absolute cosine illuminates both sides in this geometry-authoring view.
  float light = worklight != 0 ? .28+.72*abs(dot(normalize(worldNormal),normalize(eye-worldPosition))) : 1.0;
  color = vec4(albedo*light,1.0);
}
"""


class Viewport(QOpenGLWidget):
    checkpoint = Signal()
    edited = Signal()
    finished = Signal()
    selected = Signal(int)
    toolRequested = Signal(str)
    failure = Signal(str)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setMouseTracking(True)
        self.camera, self.gizmos = Camera(), Gizmos()
        self.world, self.volumes, self.selection = None, [], -1
        self.mode, self.shading = "select", "Textured + worklight"
        self.speed, self.flying, self.keys = 300, False, set()
        self.drag, self.program, self.vao, self.vbo = None, 0, 0, 0
        self.textures, self.upload_needed = {}, False
        self.overlay = Overlay(self)
        self.last_time = time.monotonic()
        self.timer = QTimer(self)
        self.timer.timeout.connect(self.tick)
        self.timer.start(16)

    def initializeGL(self):
        try:
            self.program = compileProgram(
                compileShader(VERTEX, gl.GL_VERTEX_SHADER),
                compileShader(FRAGMENT, gl.GL_FRAGMENT_SHADER),
            )
            self.vao = gl.glGenVertexArrays(1)
            self.vbo = gl.glGenBuffers(1)
            self.context().aboutToBeDestroyed.connect(self.release_resources)
            self.upload_needed = True
        except Exception as error:
            self.failure.emit(str(error))

    def release_resources(self):
        self.makeCurrent()
        if self.textures:
            gl.glDeleteTextures(list(self.textures.values()))
        if self.vbo:
            gl.glDeleteBuffers(1, [self.vbo])
        if self.vao:
            gl.glDeleteVertexArrays(1, [self.vao])
        if self.program:
            gl.glDeleteProgram(self.program)
        self.program = self.vao = self.vbo = 0
        self.textures = {}
        self.doneCurrent()

    def set_world(self, world):
        self.stop_flight()
        self.world, self.upload_needed = world, True
        self.frame_map()

    def frame_map(self):
        if self.world:
            self.camera.frame(
                [edge[i : i + 3] for edge in self.world["lines"] for i in (0, 3)],
                self.width(),
                self.height(),
            )
            self.update()

    def upload(self):
        if self.textures:
            gl.glDeleteTextures(list(self.textures.values()))
        self.textures = {}
        mesh = array("f", self.world["texturedMesh"]).tobytes()
        gl.glBindVertexArray(self.vao)
        gl.glBindBuffer(gl.GL_ARRAY_BUFFER, self.vbo)
        gl.glBufferData(gl.GL_ARRAY_BUFFER, len(mesh), mesh, gl.GL_STATIC_DRAW)
        for index, size, offset in [(0, 3, 0), (1, 3, 12), (2, 2, 24)]:
            gl.glEnableVertexAttribArray(index)
            gl.glVertexAttribPointer(
                index, size, gl.GL_FLOAT, False, 32, ctypes.c_void_p(offset)
            )
        fallback = Image.new("RGBA", (32, 32), "#6f5070")
        for y in range(32):
            for x in range(32):
                if ((x // 8) + (y // 8)) % 2:
                    fallback.putpixel((x, y), (35, 35, 40, 255))
        for name, image in self.world["images"].items():
            image = image if image is not None else fallback
            texture = gl.glGenTextures(1)
            self.textures[name] = texture
            gl.glBindTexture(gl.GL_TEXTURE_2D, texture)
            gl.glTexImage2D(
                gl.GL_TEXTURE_2D,
                0,
                gl.GL_RGBA8,
                image.width,
                image.height,
                0,
                gl.GL_RGBA,
                gl.GL_UNSIGNED_BYTE,
                image.tobytes(),
            )
            gl.glGenerateMipmap(gl.GL_TEXTURE_2D)
            gl.glTexParameteri(
                gl.GL_TEXTURE_2D, gl.GL_TEXTURE_MIN_FILTER, gl.GL_LINEAR_MIPMAP_LINEAR
            )
            gl.glTexParameteri(gl.GL_TEXTURE_2D, gl.GL_TEXTURE_MAG_FILTER, gl.GL_LINEAR)
            gl.glTexParameteri(gl.GL_TEXTURE_2D, gl.GL_TEXTURE_WRAP_S, gl.GL_REPEAT)
            gl.glTexParameteri(gl.GL_TEXTURE_2D, gl.GL_TEXTURE_WRAP_T, gl.GL_REPEAT)
        self.upload_needed = False

    def paintGL(self):
        try:
            self.draw_world()
        except Exception as error:
            self.failure.emit(str(error))

    def resizeEvent(self, event):
        super().resizeEvent(event)
        self.overlay.setGeometry(self.rect())

    def update(self, *args):
        super().update(*args)
        if hasattr(self, "overlay"):
            self.overlay.update()

    def draw_world(self):
        gl.glClearColor(0.12, 0.14, 0.16, 1)
        gl.glClear(gl.GL_COLOR_BUFFER_BIT | gl.GL_DEPTH_BUFFER_BIT)
        if not self.program or not self.world:
            return
        if self.upload_needed:
            self.upload()
        gl.glEnable(gl.GL_DEPTH_TEST)
        gl.glDisable(gl.GL_BLEND)
        gl.glDisable(gl.GL_CULL_FACE)
        gl.glUseProgram(self.program)
        gl.glBindVertexArray(self.vao)
        location = lambda name: gl.glGetUniformLocation(self.program, name)
        gl.glUniform3fv(location("eye"), 1, self.camera.position)
        for name, value in zip(("forward", "right", "up"), self.camera.basis()):
            gl.glUniform3fv(location(name), 1, value)
        gl.glUniform1f(location("aspect"), self.width() / max(1, self.height()))
        gl.glUniform1i(location("textured"), int(self.shading != "Solid + worklight"))
        gl.glUniform1i(location("worklight"), int(self.shading != "Unlit textures"))
        gl.glUniform1i(location("baseTexture"), 0)
        gl.glActiveTexture(gl.GL_TEXTURE0)
        for batch in self.world["batches"]:
            gl.glBindTexture(gl.GL_TEXTURE_2D, self.textures[batch["material"]])
            gl.glDrawArrays(gl.GL_TRIANGLES, batch["first"], batch["count"])
        gl.glBindVertexArray(0)
        gl.glUseProgram(0)

    def ray(self, point):
        return self.camera.ray(point, self.width(), self.height())

    def mousePressEvent(self, event):
        self.setFocus()
        if event.button() == Qt.MouseButton.RightButton:
            if self.drag:
                self.drag = None
                self.finished.emit()
            self.cursor_origin = QCursor.pos()
            self.flying = True
            self.setCursor(Qt.CursorShape.BlankCursor)
            self.grabMouse()
            self.warp_center()
            return
        if event.button() != Qt.MouseButton.LeftButton or self.flying:
            return
        hit = self.gizmos.hit([event.position().x(), event.position().y()])
        if not hit:
            return
        if hit["kind"] == "select":
            self.selected.emit(hit["index"])
            return
        if not 0 <= self.selection < len(self.volumes):
            return
        original = copy.deepcopy(self.volumes[self.selection])
        origin = original["capture"] if self.mode == "capture" else original["center"]
        basis = axes(original["rotationDegrees"])
        normal = (
            basis[hit["axis"]] if hit["kind"] == "rotate" else self.camera.basis()[0]
        )
        ray = self.ray([event.position().x(), event.position().y()])
        if hit["kind"] in ("axis", "face"):
            start = axis_distance(self.camera.position, ray, origin, basis[hit["axis"]])
        else:
            start = plane_point(self.camera.position, ray, origin, normal)
        if start is None:
            return
        if hit["kind"] == "rotate" and math.hypot(*sub(start, origin)) < 1e-5:
            return
        self.checkpoint.emit()
        self.drag = dict(
            hit=hit,
            original=original,
            origin=origin,
            normal=normal,
            start=start,
            index=self.selection,
            mode=self.mode,
        )
        self.grabMouse()

    def mouseMoveEvent(self, event):
        if self.flying:
            delta = event.position() - self.rect().center()
            self.camera.yaw -= delta.x() * 0.003
            self.camera.pitch = max(
                -1.55, min(1.55, self.camera.pitch - delta.y() * 0.003)
            )
            if delta.manhattanLength() > 0:
                self.warp_center()
                self.update()
            return
        if not self.drag:
            return
        drag = self.drag
        volume = self.volumes[drag["index"]]
        ray = self.ray([event.position().x(), event.position().y()])
        kind, original, origin = drag["hit"]["kind"], drag["original"], drag["origin"]
        if kind in ("axis", "face"):
            axis = drag["hit"]["axis"]
            direction = axes(original["rotationDegrees"])[axis]
            distance = axis_distance(self.camera.position, ray, origin, direction)
            if distance is None:
                return
            distance -= drag["start"]
            if kind == "face":
                volume.update(
                    resize_face(original, axis, drag["hit"]["sign"], distance)
                )
            else:
                key = "capture" if drag["mode"] == "capture" else "center"
                volume[key] = add(original[key], mul(direction, distance))
        else:
            point = plane_point(self.camera.position, ray, origin, drag["normal"])
            if point is None:
                return
            if kind == "rotate":
                a, b = unit(sub(drag["start"], origin)), unit(sub(point, origin))
                angle = math.atan2(dot(drag["normal"], cross(a, b)), dot(a, b))
                volume["rotationDegrees"] = rotate(
                    original["rotationDegrees"], drag["normal"], angle
                )
            else:
                key = "capture" if drag["mode"] == "capture" else "center"
                volume[key] = add(original[key], sub(point, drag["start"]))
        self.edited.emit()
        self.update()

    def mouseReleaseEvent(self, event):
        if event.button() == Qt.MouseButton.RightButton:
            self.stop_flight(True)
        if event.button() == Qt.MouseButton.LeftButton and self.drag:
            self.drag = None
            self.releaseMouse()
            self.finished.emit()

    def stop_flight(self, restore_cursor=False):
        if self.flying:
            self.flying = False
            self.keys.clear()
            self.releaseMouse()
            self.unsetCursor()
            if restore_cursor:
                QCursor.setPos(self.cursor_origin)

    def focusOutEvent(self, event):
        self.stop_flight()
        if self.drag:
            self.drag = None
            self.releaseMouse()
            self.finished.emit()
        super().focusOutEvent(event)

    def warp_center(self):
        QCursor.setPos(self.mapToGlobal(self.rect().center()))

    def keyPressEvent(self, event):
        if event.key() == Qt.Key.Key_Escape:
            self.stop_flight(True)
        elif self.flying:
            self.keys.add(event.key())
        else:
            tool = {
                Qt.Key.Key_Q: "select",
                Qt.Key.Key_W: "move",
                Qt.Key.Key_E: "rotate",
                Qt.Key.Key_R: "resize",
                Qt.Key.Key_T: "capture",
            }.get(event.key())
            if tool:
                self.toolRequested.emit(tool)
            else:
                super().keyPressEvent(event)

    def keyReleaseEvent(self, event):
        if not event.isAutoRepeat():
            self.keys.discard(event.key())

    def wheelEvent(self, event):
        self.speed = max(
            10, min(10000, self.speed * math.exp(event.angleDelta().y() * 0.0015))
        )
        self.update()

    def tick(self):
        now = time.monotonic()
        dt, self.last_time = min(0.05, now - self.last_time), now
        if not self.flying:
            return
        k = lambda key: int(key in self.keys)
        forward, right, _ = self.camera.basis()
        motion = add(
            add(
                mul(forward, k(Qt.Key.Key_W) - k(Qt.Key.Key_S)),
                mul(right, k(Qt.Key.Key_D) - k(Qt.Key.Key_A)),
            ),
            [0, 0, k(Qt.Key.Key_E) - k(Qt.Key.Key_Q)],
        )
        if math.hypot(*motion):
            speed = self.speed * (3 if Qt.Key.Key_Shift in self.keys else 1)
            self.camera.position = add(
                self.camera.position, mul(unit(motion), speed * dt)
            )
            self.update()
