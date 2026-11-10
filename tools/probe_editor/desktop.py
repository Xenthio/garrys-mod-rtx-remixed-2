"""Native cubemap authoring application. No browser, web server, or embedded HTML."""

import argparse
import copy
import json
from pathlib import Path
import sys

from PySide6.QtCore import Qt, QTimer, QSize, QSettings
from PySide6.QtGui import (
    QAction,
    QActionGroup,
    QColor,
    QIcon,
    QKeySequence,
    QPalette,
    QSurfaceFormat,
)
from PySide6.QtWidgets import (
    QApplication,
    QCheckBox,
    QComboBox,
    QDockWidget,
    QDoubleSpinBox,
    QFileDialog,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QMainWindow,
    QMessageBox,
    QScrollArea,
    QSpinBox,
    QToolBar,
    QWidget,
)

from . import document, geometry, storage
from .math3d import corners
from .native_view import Viewport

TOOLS = [
    ("select", "Select", "Q", "select_tool_icon.png"),
    ("move", "Move bounds", "W", "move_tool_icon.png"),
    ("rotate", "Rotate bounds", "E", "rotate_tool_icon.png"),
    ("resize", "Resize faces", "R", "scale_tool_icon.png"),
    ("capture", "Move capture", "T", "move_tool_icon.png"),
]


def apply_palette(app):
    app.setStyle("Fusion")
    palette = QPalette()
    for role, value in [
        ("Window", "#303236"),
        ("WindowText", "#e2e3e5"),
        ("Base", "#25272a"),
        ("AlternateBase", "#303236"),
        ("Text", "#e2e3e5"),
        ("Button", "#373a3f"),
        ("ButtonText", "#e2e3e5"),
        ("Highlight", "#36688a"),
        ("HighlightedText", "#ffffff"),
    ]:
        palette.setColor(getattr(QPalette.ColorRole, role), QColor(value))
    app.setPalette(palette)


class Editor(QMainWindow):
    def __init__(self, args):
        super().__init__()
        self.args = args
        self.output, self.runtime = args.output, args.runtime_output
        self.world, self.data, self.selection = None, None, -1
        self.history, self.future, self.dirty = [], [], False
        self.resize(1440, 900)
        self.setDockOptions(
            QMainWindow.DockOption.AllowTabbedDocks
            | QMainWindow.DockOption.AllowNestedDocks
        )
        self.view = Viewport(self)
        self.setCentralWidget(self.view)
        self.view.checkpoint.connect(self.checkpoint)
        self.view.edited.connect(lambda: self.changed(False))
        self.view.finished.connect(self.properties)
        self.view.selected.connect(self.select)
        self.view.toolRequested.connect(self.set_tool)
        self.view.failure.connect(
            lambda error: self.statusBar().showMessage("OpenGL: " + error)
        )
        self.live_timer = QTimer(self)
        self.live_timer.setSingleShot(True)
        self.live_timer.setInterval(500)
        self.live_timer.timeout.connect(lambda: self.save(False))
        self.make_actions()
        self.make_docks()
        self.settings = (
            QSettings("OpenRemix", "CubemapWorkbench")
            if getattr(args, "persist_layout", True)
            else None
        )
        if self.settings:
            if self.settings.contains("geometry"):
                self.restoreGeometry(self.settings.value("geometry"))
            if self.settings.contains("layout"):
                self.restoreState(self.settings.value("layout"))
        self.load_map(args.bsp)

    def action(self, label, callback, shortcut=None):
        result = QAction(label, self)
        result.triggered.connect(callback)
        if shortcut:
            result.setShortcut(shortcut)
        return result

    def make_actions(self):
        file_menu = self.menuBar().addMenu("&File")
        file_menu.addAction(
            self.action("Open BSP…", self.open_map, QKeySequence.StandardKey.Open)
        )
        file_menu.addAction(
            self.action("Save", lambda: self.save(True), QKeySequence.StandardKey.Save)
        )
        file_menu.addAction(self.action("Save authoring JSON as…", self.save_as))
        file_menu.addAction(self.action("Import authoring JSON…", self.import_json))
        file_menu.addSeparator()
        file_menu.addAction(
            self.action("Exit", self.close, QKeySequence.StandardKey.Quit)
        )
        edit_menu = self.menuBar().addMenu("&Edit")
        self.undo_action = self.action("Undo", self.undo, QKeySequence.StandardKey.Undo)
        self.redo_action = self.action("Redo", self.redo, QKeySequence.StandardKey.Redo)
        edit_menu.addActions([self.undo_action, self.redo_action])
        edit_menu.addAction(self.action("Add cubemap at camera", self.add_volume))
        edit_menu.addAction(self.action("Delete selected", self.remove_volume))
        view_menu = self.menuBar().addMenu("&View")
        self.view_menu = view_menu
        view_menu.addAction(self.action("Frame selected", self.frame_selected, "F"))
        view_menu.addAction(self.action("Go to capture", self.go_capture))
        view_menu.addAction(self.action("Frame map", self.view.frame_map))
        toolbar = QToolBar("Tools", self)
        toolbar.setObjectName("tool-strip")
        toolbar.setMovable(False)
        toolbar.setIconSize(QSize(28, 28))
        toolbar.setToolButtonStyle(Qt.ToolButtonStyle.ToolButtonTextUnderIcon)
        self.addToolBar(Qt.ToolBarArea.LeftToolBarArea, toolbar)
        group = QActionGroup(self)
        group.setExclusive(True)
        self.tool_actions = {}
        for key, label, shortcut, icon in TOOLS:
            action = self.action(label, lambda checked=False, k=key: self.set_tool(k))
            action.setCheckable(True)
            action.setToolTip(f"{label} ({shortcut}, while viewport focused)")
            if self.args.icon_directory and (self.args.icon_directory / icon).is_file():
                action.setIcon(QIcon(str(self.args.icon_directory / icon)))
            group.addAction(action)
            toolbar.addAction(action)
            self.tool_actions[key] = action
        self.tool_actions["select"].setChecked(True)
        toolbar.addSeparator()
        toolbar.addAction(self.action("+ Cubemap", self.add_volume))
        toolbar.addAction(self.action("Delete", self.remove_volume))
        top = QToolBar("Viewport", self)
        top.setObjectName("viewport-toolbar")
        top.setMovable(False)
        self.addToolBar(Qt.ToolBarArea.TopToolBarArea, top)
        self.shading = QComboBox()
        self.shading.addItems(
            ["Textured + worklight", "Unlit textures", "Solid + worklight"]
        )
        self.shading.currentTextChanged.connect(self.set_shading)
        top.addWidget(self.shading)
        top.addSeparator()
        self.live = QCheckBox("Live save to game")
        self.live.setChecked(bool(self.runtime))
        self.live.toggled.connect(
            lambda checked: self.live_timer.start()
            if checked and self.dirty
            else self.live_timer.stop()
        )
        top.addWidget(self.live)
        top.addSeparator()
        top.addAction(self.action("Frame selected", self.frame_selected))
        top.addAction(self.action("Go to capture", self.go_capture))

    def make_docks(self):
        self.object_list = QListWidget()
        self.object_list.currentRowChanged.connect(self.select)
        dock = QDockWidget("Cubemap volumes", self)
        dock.setObjectName("volumes-dock")
        dock.setWidget(self.object_list)
        self.addDockWidget(Qt.DockWidgetArea.RightDockWidgetArea, dock)
        self.form = QFormLayout()
        self.form.setFieldGrowthPolicy(
            QFormLayout.FieldGrowthPolicy.AllNonFixedFieldsGrow
        )
        content = QWidget()
        content.setLayout(self.form)
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setWidget(content)
        properties = QDockWidget("Object properties", self)
        properties.setObjectName("properties-dock")
        properties.setMinimumWidth(320)
        properties.setWidget(scroll)
        self.addDockWidget(Qt.DockWidgetArea.RightDockWidgetArea, properties)
        self.splitDockWidget(dock, properties, Qt.Orientation.Vertical)
        self.info = QLabel("")
        self.info.setWordWrap(True)
        details = QDockWidget("Map information", self)
        details.setObjectName("map-info-dock")
        details.setWidget(self.info)
        self.addDockWidget(Qt.DockWidgetArea.BottomDockWidgetArea, details)
        for panel in (dock, properties, details):
            self.view_menu.addAction(panel.toggleViewAction())

    def load_map(self, path):
        # Load into temporaries. A malformed map must not discard current edits.
        world = geometry.load(
            Path(path), self.args.game, True, getattr(self.args, "content_root", [])
        )
        data = document.empty(world["name"], world["sha256"])
        if self.args.probe_report:
            data = document.from_probe_report(
                self.args.probe_report.read_bytes(), world["name"], world["sha256"]
            )
        if self.output.exists():
            data = document.validate(
                json.loads(self.output.read_text(encoding="utf-8")), world["sha256"]
            )
        self.live_timer.stop()
        self.world, self.data, self.history, self.future, self.dirty = (
            world,
            data,
            [],
            [],
            False,
        )
        self.args.bsp = Path(path)
        self.view.set_world(world)
        self.select(0 if data["volumes"] else -1)
        loaded = sum(image is not None for image in world["images"].values())
        self.info.setText(
            f"{world['name']} | {len(world['texturedMesh'])//24:,} triangles | "
            f"{loaded}/{len(world['images'])} materials | {world['note']}\nJSON: {self.output}\n"
            f"Runtime: {self.runtime or 'not configured'}"
        )
        self.info.setToolTip("\n".join(world["warnings"]))
        self.update_title()
        self.statusBar().showMessage(
            "Ready — select a tool on the left; right mouse + WASD flies the camera"
        )

    def update_title(self):
        name = self.world["name"] if self.world else "No map"
        self.setWindowTitle(f"{'* ' if self.dirty else ''}{name} — Cubemap Workbench")
        self.undo_action.setEnabled(bool(self.history))
        self.redo_action.setEnabled(bool(self.future))

    def refresh_list(self):
        self.object_list.blockSignals(True)
        self.object_list.clear()
        for volume in self.data["volumes"]:
            self.object_list.addItem(f"{volume['name']}   [{volume['priority']}]")
        self.object_list.setCurrentRow(self.selection)
        self.object_list.blockSignals(False)

    def select(self, index):
        if self.data is None:
            return
        self.selection = index if 0 <= index < len(self.data["volumes"]) else -1
        self.view.volumes, self.view.selection = self.data["volumes"], self.selection
        self.refresh_list()
        self.properties()
        self.view.update()

    def properties(self):
        while self.form.rowCount():
            self.form.removeRow(0)
        if self.selection < 0:
            self.form.addRow(
                QLabel("Add a cubemap at the camera or select one in the list.")
            )
            return
        volume = self.data["volumes"][self.selection]
        name = QLineEdit(volume["name"])
        name.setMaxLength(200)
        name.editingFinished.connect(lambda: self.property_change("name", name.text()))
        self.form.addRow("Name", name)
        priority = QSpinBox()
        priority.setRange(-32768, 32767)
        priority.setValue(volume["priority"])
        priority.valueChanged.connect(
            lambda value: self.property_change("priority", value)
        )
        self.form.addRow("Priority", priority)
        for key, label in [
            ("center", "Bounds center"),
            ("halfSize", "Half size"),
            ("rotationDegrees", "Rotation °"),
            ("capture", "Capture point"),
        ]:
            group = QWidget()
            row = QHBoxLayout(group)
            row.setContentsMargins(0, 0, 0, 0)
            for axis in range(3):
                field = QDoubleSpinBox()
                field.setRange(0.5 if key == "halfSize" else -1e8, 1e8)
                field.setDecimals(3)
                field.setSingleStep(1)
                field.setKeyboardTracking(False)
                field.setPrefix("XYZ"[axis] + " ")
                field.setValue(volume[key][axis])
                field.valueChanged.connect(
                    lambda value, k=key, a=axis: self.property_change(k, value, a)
                )
                row.addWidget(field)
            self.form.addRow(label, group)

    def property_change(self, key, value, axis=None):
        volume = self.data["volumes"][self.selection]
        old = volume[key] if axis is None else volume[key][axis]
        if old == value:
            return
        self.checkpoint()
        if axis is None:
            volume[key] = value
        else:
            volume[key][axis] = value
        self.changed(False)
        self.refresh_list()

    def checkpoint(self):
        self.history.append(copy.deepcopy(self.data))
        self.history = self.history[-100:]
        self.future = []

    def changed(self, rebuild=True):
        self.dirty = True
        self.view.volumes = self.data["volumes"]
        if rebuild:
            self.select(self.selection)
        self.update_title()
        self.view.update()
        self.statusBar().showMessage("Unsaved changes")
        if self.live.isChecked():
            self.live_timer.start()

    def add_volume(self):
        self.checkpoint()
        volume = document.new_volume(self.view.camera.position)
        volume["name"] = f"Cubemap {len(self.data['volumes'])+1}"
        self.data["volumes"].append(volume)
        self.selection = len(self.data["volumes"]) - 1
        self.set_tool("resize")
        self.changed()

    def remove_volume(self):
        if self.selection < 0:
            return
        self.checkpoint()
        self.data["volumes"].pop(self.selection)
        self.selection = min(self.selection, len(self.data["volumes"]) - 1)
        self.changed()

    def undo(self):
        if self.history:
            self.future.append(copy.deepcopy(self.data))
            self.data = self.history.pop()
            self.selection = min(self.selection, len(self.data["volumes"]) - 1)
            self.changed()

    def redo(self):
        if self.future:
            self.history.append(copy.deepcopy(self.data))
            self.data = self.future.pop()
            self.selection = min(self.selection, len(self.data["volumes"]) - 1)
            self.changed()

    def set_tool(self, mode):
        if mode in self.tool_actions:
            self.tool_actions[mode].setChecked(True)
            self.view.mode = mode
            self.view.update()

    def set_shading(self, mode):
        self.view.shading = mode
        self.view.update()

    def frame_selected(self):
        if self.selection >= 0:
            volume = self.data["volumes"][self.selection]
            self.view.camera.frame(
                corners(volume) + [volume["capture"]],
                self.view.width(),
                self.view.height(),
            )
            self.view.update()

    def go_capture(self):
        if self.selection >= 0:
            self.view.camera.position = list(
                self.data["volumes"][self.selection]["capture"]
            )
            self.view.update()

    def save(self, explicit=False):
        self.live_timer.stop()
        try:
            self.data = storage.save(
                self.data, self.world["sha256"], self.output, self.runtime
            )
            self.view.volumes = self.data["volumes"]
            self.dirty = False
            self.update_title()
            self.statusBar().showMessage(f"Saved {self.output}")
            return True
        except (OSError, ValueError) as error:
            self.statusBar().showMessage(str(error))
            if explicit:
                QMessageBox.warning(self, "Cannot save", str(error))
            return False

    def confirm_discard(self):
        if not self.dirty:
            return True
        self.live_timer.stop()
        choice = QMessageBox.question(
            self,
            "Unsaved cubemaps",
            "Save changes before continuing?",
            QMessageBox.StandardButton.Save
            | QMessageBox.StandardButton.Discard
            | QMessageBox.StandardButton.Cancel,
        )
        accepted = choice == QMessageBox.StandardButton.Discard or (
            choice == QMessageBox.StandardButton.Save and self.save(True)
        )
        if not accepted and self.live.isChecked():
            self.live_timer.start()
        return accepted

    def open_map(self):
        if not self.confirm_discard():
            return
        path, _ = QFileDialog.getOpenFileName(
            self, "Open Source map", str(self.args.bsp.parent), "Source BSP (*.bsp)"
        )
        if not path:
            return
        old = self.output, self.runtime, self.args.probe_report
        self.output = self.output.with_name(Path(path).stem + ".json")
        self.runtime = (
            self.runtime.with_name(Path(path).stem + ".cubemaps.bin")
            if self.runtime
            else None
        )
        self.args.probe_report = None
        try:
            self.load_map(path)
        except (OSError, ValueError) as error:
            self.output, self.runtime, self.args.probe_report = old
            QMessageBox.warning(self, "Cannot open map", str(error))

    def save_as(self):
        path, _ = QFileDialog.getSaveFileName(
            self, "Save authoring JSON", str(self.output), "Cubemap JSON (*.json)"
        )
        if path:
            previous = self.output
            self.output = Path(path)
            if not self.save(True):
                self.output = previous

    def import_json(self):
        path, _ = QFileDialog.getOpenFileName(
            self,
            "Import authoring JSON",
            str(self.output.parent),
            "Cubemap JSON (*.json)",
        )
        if not path:
            return
        try:
            incoming = document.validate(
                json.loads(Path(path).read_text(encoding="utf-8")), self.world["sha256"]
            )
            self.checkpoint()
            self.data = incoming
            self.selection = 0 if incoming["volumes"] else -1
            self.changed()
        except (OSError, ValueError) as error:
            QMessageBox.warning(self, "Cannot import", str(error))

    def closeEvent(self, event):
        self.view.stop_flight()
        if self.confirm_discard():
            self.live_timer.stop()
            if self.settings:
                self.settings.setValue("geometry", self.saveGeometry())
                self.settings.setValue("layout", self.saveState())
            event.accept()
        else:
            event.ignore()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bsp", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--runtime-output", type=Path)
    parser.add_argument(
        "--game",
        type=Path,
        help="Game install root containing garrysmod and sourceengine",
    )
    parser.add_argument(
        "--content-root",
        type=Path,
        action="append",
        default=[],
        help="Additional material/VPK root; repeat for mounted games",
    )
    parser.add_argument("--probe-report", type=Path)
    parser.add_argument(
        "--icon-directory",
        type=Path,
        help="Optional local Hammer tools/images/hammer directory",
    )
    args = parser.parse_args()
    surface = QSurfaceFormat()
    surface.setVersion(3, 3)
    surface.setProfile(QSurfaceFormat.OpenGLContextProfile.CoreProfile)
    surface.setDepthBufferSize(24)
    surface.setSamples(4)
    QSurfaceFormat.setDefaultFormat(surface)
    app = QApplication(sys.argv[:1])
    apply_palette(app)
    try:
        window = Editor(args)
    except (OSError, ValueError) as error:
        QMessageBox.critical(None, "Cannot open map", str(error))
        return 1
    window.show()
    QTimer.singleShot(0, window.view.frame_map)
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
