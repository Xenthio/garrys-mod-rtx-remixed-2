"""Owned Qt application integration test; uses a caller-selected BSP read-only."""

import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import sys
import tempfile
import traceback

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "_work/deps/probe-editor"), str(ROOT / "tools")]
from PySide6.QtCore import QEvent, QPointF, QTimer, Qt
from PySide6.QtGui import QMouseEvent, QSurfaceFormat
from PySide6.QtWidgets import QApplication
from probe_editor.desktop import Editor, apply_palette
from probe_editor import math3d as m


def mouse(view, kind, point):
    buttons = (
        Qt.MouseButton.NoButton
        if kind == QEvent.Type.MouseButtonRelease
        else Qt.MouseButton.LeftButton
    )
    button = (
        Qt.MouseButton.NoButton
        if kind == QEvent.Type.MouseMove
        else Qt.MouseButton.LeftButton
    )
    local = QPointF(*point[:2])
    global_position = QPointF(view.mapToGlobal(local.toPoint()))
    event = QMouseEvent(
        kind, local, global_position, button, buttons, Qt.KeyboardModifier.NoModifier
    )
    if kind == QEvent.Type.MouseButtonPress:
        view.mousePressEvent(event)
    elif kind == QEvent.Type.MouseButtonRelease:
        view.mouseReleaseEvent(event)
    else:
        view.mouseMoveEvent(event)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bsp", type=Path, required=True)
    parser.add_argument("--game", type=Path, required=True)
    parser.add_argument("--report-dir", type=Path, required=True)
    parser.add_argument("--icon-directory", type=Path)
    args = parser.parse_args()
    args.report_dir.mkdir(parents=True, exist_ok=True)
    format = QSurfaceFormat()
    format.setVersion(3, 3)
    format.setProfile(QSurfaceFormat.OpenGLContextProfile.CoreProfile)
    format.setDepthBufferSize(24)
    format.setSamples(4)
    QSurfaceFormat.setDefaultFormat(format)
    app = QApplication([])
    apply_palette(app)
    failures, checks = [], []
    sys.excepthook = lambda kind, error, tb: (
        failures.append(str(error)),
        traceback.print_exception(kind, error, tb),
    )
    result = [1]
    with tempfile.TemporaryDirectory() as folder:
        options = argparse.Namespace(
            bsp=args.bsp,
            game=args.game,
            probe_report=None,
            icon_directory=args.icon_directory,
            output=Path(folder) / "bounds.json",
            runtime_output=Path(folder) / "bounds.bin",
            persist_layout=False,
        )
        window = Editor(options)
        window.view.failure.connect(failures.append)
        window.live.setChecked(False)
        window.show()

        def run():
            try:
                view = window.view
                assert view.isValid(), "OpenGL context failed"
                view.camera.position = [-1900, -1900, 200]
                window.add_volume()
                original = window.data["volumes"][0]
                original.update(
                    center=[-1550, -1536, 0],
                    halfSize=[90, 110, 70],
                    rotationDegrees=[17, -23, 31],
                    capture=[-1550, -1536, 0],
                )
                original = copy.deepcopy(original)
                delta = m.sub(original["center"], view.camera.position)
                view.camera.yaw = math.atan2(delta[1], delta[0])
                view.camera.pitch = math.asin(delta[2] / math.hypot(*delta))
                project = lambda point: view.camera.project(
                    point, view.width(), view.height()
                )
                for mode in ("move", "resize", "capture", "rotate"):
                    window.data["volumes"][0] = copy.deepcopy(original)
                    window.select(0)
                    window.set_tool(mode)
                    view.overlay.repaint()
                    app.processEvents()
                    kind = (
                        "rotate"
                        if mode == "rotate"
                        else "face"
                        if mode == "resize"
                        else "axis"
                    )
                    candidates = [h for h in view.gizmos.handles if h["kind"] == kind]
                    assert candidates, f"No {mode} handles"
                    chosen = None
                    for handle in candidates:
                        point = handle.get("point")
                        if point is None:
                            a, b = handle["segment"]
                            point = m.mul(m.add(a[:2], b[:2]), 0.5)
                        hit = view.gizmos.hit(point[:2])
                        if hit is None or hit["kind"] != kind:
                            continue
                        mouse(view, QEvent.Type.MouseButtonPress, point)
                        if view.drag:
                            chosen, point = view.drag, point
                            break
                    assert chosen, f"{mode} handle cannot begin a drag"
                    axis = m.axes(original["rotationDegrees"])[chosen["hit"]["axis"]]
                    if kind in ("axis", "face"):
                        end = project(
                            m.add(chosen["origin"], m.mul(axis, chosen["start"] + 25))
                        )
                    else:
                        v = m.sub(chosen["start"], chosen["origin"])
                        turned = m.add(
                            m.add(
                                m.mul(v, math.cos(0.3)),
                                m.mul(m.cross(axis, v), math.sin(0.3)),
                            ),
                            m.mul(axis, m.dot(axis, v) * (1 - math.cos(0.3))),
                        )
                        end = project(m.add(chosen["origin"], turned))
                    assert end is not None
                    mouse(view, QEvent.Type.MouseMove, end)
                    mouse(view, QEvent.Type.MouseButtonRelease, end)
                    actual = window.data["volumes"][0]
                    assert actual != original, f"{mode} drag had no effect"
                    if mode == "capture":
                        assert (
                            actual["center"] == original["center"]
                            and actual["rotationDegrees"] == original["rotationDegrees"]
                        )
                    else:
                        assert (
                            actual["capture"] == original["capture"]
                        ), f"{mode} moved capture point"
                    if mode == "resize":
                        sign, index = chosen["hit"]["sign"], chosen["hit"]["axis"]
                        before = m.sub(
                            original["center"],
                            m.mul(axis, sign * original["halfSize"][index]),
                        )
                        after = m.sub(
                            actual["center"],
                            m.mul(axis, sign * actual["halfSize"][index]),
                        )
                        assert math.hypot(*m.sub(before, after)) < 1e-5
                    window.undo()
                    assert window.data["volumes"][0] == original, f"{mode} undo failed"
                    checks.append(mode + " drag and undo")
                window.data["volumes"][0] = copy.deepcopy(original)
                window.select(0)
                window.set_tool("rotate")
                frame_hashes = set()
                for shading in (
                    "Unlit textures",
                    "Solid + worklight",
                    "Textured + worklight",
                ):
                    window.shading.setCurrentText(shading)
                    view.repaint()
                    frame = view.grabFramebuffer()
                    assert not frame.isNull(), shading + " framebuffer missing"
                    frame_hashes.add(hashlib.sha256(bytes(frame.bits())).hexdigest())
                    frame.save(
                        str(args.report_dir / (shading.replace(" ", "-") + ".png"))
                    )
                window.grab().save(str(args.report_dir / "window.png"))
                assert window.save(False), "Native save failed"
                assert options.runtime_output.read_bytes()[:8] == b"BSPCUBE1"
                checks.append("JSON and live sidecar save")
                checks.append("all three shading modes rendered")
                assert len(frame_hashes) == 3, "Shading modes produced identical pixels"
                assert not failures, failures
                result[0] = 0
                print("Native editor: " + ", ".join(checks), flush=True)
            except Exception:
                traceback.print_exc()
            finally:
                window.dirty = False
                window.close()
                app.quit()

        QTimer.singleShot(1000, run)
        QTimer.singleShot(20000, app.quit)
        app.exec()
    (args.report_dir / "result.json").write_text(
        json.dumps(dict(exit=result[0], checks=checks, errors=failures), indent=2)
    )
    return result[0]


if __name__ == "__main__":
    raise SystemExit(main())
