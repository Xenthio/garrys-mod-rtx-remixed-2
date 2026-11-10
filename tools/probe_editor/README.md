# Cubemap bounds workbench

The default editor is a native Qt application with an OpenGL viewport, dockable object list and
properties, File/Edit/View menus, and a Hammer-style left tool strip. It does not run a browser or
HTTP server. The launcher installs pinned Python dependencies into the repository's ignored
`_work/deps/probe-editor` directory on first use:

```powershell
./tools/launch_cubemap_editor.ps1 gm_construct
```

An absolute BSP path also works. The configured game root supplies loose materials and VPK content;
BSP pakfile assets take precedence. If Half-Life Alyx is installed beside the configured GMod install,
its local Hammer icons are used. `-IconDirectory` selects another local Hammer icon folder. No Valve
images are bundled with this tool. Native dark colors and dock geometry are owned by the app; Alyx
stylesheets are not applied wholesale because they target Valve-specific Qt widgets.
Additional mounted content folders containing `materials/` or VPK directories can be supplied with
`-ContentRoots` on the launcher or repeated `--content-root` module arguments. Packed GMA addons
are not indexed automatically; extract their materials or point at an existing loose content folder.

The left toolbar provides Select (Q), Move bounds (W), Rotate bounds (E), Resize faces (R), and Move
capture (T). Tool keys apply while the viewport has focus. Move and capture modes provide axis
handles plus a center handle for dragging in the view plane. Rotation uses three local-axis rings.
Resize provides six face handles and keeps the opposite face fixed. Changing bounds never moves the
capture point. Numeric fields expose every transform and signed overlap priority. Add cubemap places
a new volume at the camera. Undo/redo and Ctrl+S are available.

Hold right mouse and use WASD to fly; Q/E move vertically, Shift boosts speed and the wheel adjusts
speed. Escape ends mouse look. Frame selected fits the bounds to the viewport; Go to capture moves
the camera to the selected reflection origin. The viewport offers Textured + worklight, Unlit textures,
and Solid + worklight. The worklight is an authoring aid, not a preview of baked game illumination.

Open another map using File > Open BSP. Save atomically writes the authoring JSON and optionally the
map-fingerprinted runtime sidecar. The previous files remain intact if runtime validation fails.
Dock layout and window placement persist between sessions. Import checks the BSP SHA-256 fingerprint
before replacing the editable document. Closing or opening a map offers Save/Discard/Cancel when needed.

For direct invocation, install `requirements-desktop.txt`, then run from `tools`:

```powershell
python -m probe_editor "D:/maps/example.bsp" --game "D:/games/GarrysMod" --output "D:/bounds/example.json"
```

To start from a published automatic fit, add `--probe-report "D:/reports/probes.json"`. Select the
report for this BSP explicitly: native probe reports currently contain no map fingerprint. Coordinates
are imported in host units without applying `metersPerUnit` again. An unchanged dashed orange overlay
shows the original fit while you edit; the Original fit checkbox hides it. Stable source IDs and the
reference report fingerprint persist in saved documents for later fitting comparisons. An existing
output file retains your edits when opened with a reference report.

Coordinates use BSP units. `center`, positive `halfSize`, and `rotationDegrees` describe an oriented
box. Rotation applies X then Y then Z (`Rz * Ry * Rx`); `capture` is independent in world space.
`id` is stable across edits. Larger integer `priority` is intended to win overlapping regions.

The preview displays textured world-brush surfaces and displacement base polygons, excluding sky and
nodraw faces. Props, displacement tessellation, material proxies, animated textures, water shaders,
and PBR replacements are not displayed. Supported VTF previews include common RGBA/RGB/BGRA and
DXT1/3/5 formats, with bounded mip decoding. Unsupported or missing materials use a checker texture;
the map information tooltip lists failures. OpenGL 3.3 and Python 3.10+ are required.

The original browser wireframe workbench remains an explicit diagnostic fallback via `-Browser`
on the launcher or `--browser` on the module. It keeps its orthographic precision views; new native
features are not duplicated there. `--no-browser` starts that diagnostic server without opening a tab.

For the configured testing install, launch from the repository root:

```powershell
./tools/launch_cubemap_editor.ps1 gm_construct
```

An absolute BSP path also works. Add `-ProbeReport <path>` to start from an explicitly selected
native probe report. Authoring JSON stays in `_work/cubemaps`; a compact runtime sidecar is published
to the testing install's `garrysmod/data/openremix/cubemaps/<map>.cubemaps.bin`. Live save is enabled
when a runtime output is configured. Edits publish after a 500 ms pause; Ctrl+S still saves explicitly.
The game polls twice per second and verifies the sidecar's SHA-256 against its loaded BSP.
A different BSP revision or malformed edit preserves the last valid request. Removing the sidecar
restores automatic cubemaps; saving an empty document explicitly disables local cubemaps.

For another install, invoke `python -m probe_editor` with an explicit `--runtime-output <path>`.
Independent sampling metadata is optional in each JSON volume:

```json
"sampling": {
  "minimum": [-160, -128, -96],
  "maximum": [160, 128, 96],
  "blendDistance": 16
}
```

These bounds use the box's local axes and center, independently of its projection half size.
The capture stays constrained to the projection box, and need not lie inside influence bounds.
Blend distance is in BSP units; zero derives a 20%-of-half-extent feather band.
Probe-report imports preserve representable influence bounds and feathering, rebased into the editor's
centered frame. Unsupported independent axis feather widths are rejected rather than silently changed.
The current viewport handles still edit projection bounds; independent influence handles remain future work.

Documents without sampling changes keep the `BSPCUBE1` runtime format. Extended documents use
`BSPCUBE2`, with the original 96-byte volume followed by a 28-byte sampling record. Mixed documents
preserve the legacy half-metre feather for unchanged volumes. Extended files require both the updated
adapter and a runtime supporting `REMIXAPI_REFLECTION_VOLUMES_SAMPLING_VERSION_EXT`; a rejected
publication retains the previous valid override. Existing sidecars are not rewritten automatically.

The adapter's `OPENREMIX_PROXY_CUBEMAP_BOUNDS` environment variable can select a diagnostic sidecar
instead of the default map-specific path. The JSON itself is not parsed by the game.

`references/gm_construct.json` preserves the user's two authored volumes for the skylit main
space and adjoining area. Use these as a bounds-fitting reference, not as complete map coverage.
Keep the independent capture positions and priorities (1 and 0), and check the stored BSP
fingerprint before comparing or publishing. The file does not replace the live authoring document
in `_work/cubemaps`; later edits there remain the user's working copy. Further viewport/editor
polish is deferred to the planned C# tooling work.

The matching OpenRemix SDK/runtime supports at most 64 authored volumes, and capture points must remain inside
their projection bounds. Rotation and signed overlap priority are supported. Bounds/priority changes
reuse filtered radiance without new rays. Moving or adding a capture traces only the affected cubemap;
scene acceleration is reused during the authoring session. Completed lightmaps and diffuse probes
stay active throughout. The previous reflection set stays visible until a complete replacement is ready.
The native API contains generic volumes and scene IDs; BSP fingerprints and units belong to the adapter.


Checks, from the repository root:

```powershell
python tests/probe_editor.py
node tests/probe_editor_rotation.mjs
node tests/probe_editor_perspective.mjs
node tests/probe_editor_flight.mjs
python tests/probe_editor_assets.py
```

Native rendering and drag integration (opens and closes its own test window; outputs are local):

```powershell
python tests/probe_editor_desktop.py --bsp "D:/maps/example.bsp" --game "D:/games/GarrysMod" --report-dir "_work/reports/editor-check"
```

### BSP projection reference extraction

From the `tools` directory, run:

```powershell
python -m probe_editor.bsp_reflections "D:\maps\bm_c1a1b.bsp" --output "reference.json"
```

This reads the bounded entity lump (including supported Source LZMA compression), fingerprints the
complete BSP and selects only `env_cubemap` entities with explicit `parallax=1`. The reference keeps
capture origins, raw box offsets, source angles and optional Hammer side assignments. It preserves
outside-box capture points as reference facts rather than silently moving or dropping them.

The output is a reference corpus, not an authoring document or runtime sidecar. Projection offsets
still need their coordinate/rotation convention checked against the source renderer. Hammer side IDs
are not compiled BSP face indices, and neither influence bounds nor overlap priorities can be inferred
from these fields alone. The extractor deliberately leaves those policies unspecified.

Add `--compiled-receivers` to include actual compiled face-to-material assignments. Add
`--capture-assignments` to also inspect packed material patches for explicit capture-texture links
(this includes compiled receivers automatically). The latter recognizes bounded compiler-style
quoted `patch` / `include` / `replace` scalar blocks, including extra replacements such as crack
materials. It does not resolve general VMT inheritance, conditions or proxies. Each receiver material
gets a status: missing packed material, unsupported form, nonlocal capture, ordinary capture without
an authored projection, unique authored projection, or ambiguous projection origins. Duplicate packed
material names and exceeded read budgets reject extraction. No association is guessed from a material
filename. Compiled indices remain scoped to the fingerprinted BSP; these links do not define dynamic
receiver influence or reflection blending.

### Inspect published volumes against brush geometry

From the repository root, `python tools/probe_bounds.py map.bsp probes.json section.png --slice -90`
plots a world-space brush section and published probe bounds. Use `--axis x`, `--axis y` or the
default `--axis z` to choose the slice plane, and repeat `--probe INDEX` to select volumes.
`--window LEFT RIGHT BOTTOM TOP` zooms the two plotted world axes. `--receiver X Y Z` marks a
world-space receiver projected onto the slice.

Solid polygons show parallax projection bounds; dashed polygons show independent influence bounds
when they differ. Both follow the published rigid transform, including translated and oblique boxes.
The plot omits props, moving brushes and displacements: an open region in this diagram does not by
itself establish visibility or reflection quality. Run `python tests/probe_bounds.py` for transform
and section regression coverage.
