> This project uses AI-generated code

# Garry's Mod RTX Remixed 2
This is currently a proxy `shaderapidx9.dll` for Garry's Mod that wraps the
Source shader API (and later D3D9) to submit the game's rendering to
[OpenRemix](https://github.com/OpenRenderCollective/OpenRemix) through the Remix C API. 

This removes the need to force the engine to run in the fixed function pipeline, so all shader-based systems are supported and are translated when applicable. OpenRemix has its own rasterization pass for sprites and other effects that would not work well in path-tracing and can be integrated seamlessly.

RTX Remix mods from the old version of the project will be supported, and also any lua stools like the Remix API lights, etc.

Multiplayer works out of the box since everything lives in the wrapper.

Other source game support is planned

## Build
This is primarily built on Linux, Windows support is largely untested.

    meson setup --cross-file cross/x86_64-w64-mingw32.txt _build
    meson compile -C _build

Deploy: rename `bin/win64/shaderapidx9.dll` to `shaderapidx9_orig.dll` and copy
`_build/shaderapidx9.dll` in its place.

## Isolated live-game testing (Windows)

`tools/stage_game.py` prepares a separate `gm_construct`/`gm_flatgrass` test installation. It
copies selected engine binaries, base VPK packages, loose scripts/assets and the
built renderer into a new directory. Allow several GB of disk space. Existing
destinations are refused, and installed files are never replaced or linked.
The source installation must already contain `shaderapidx9_orig.dll`.

```powershell
python tools/stage_game.py --source 'E:/SteamLibrary/steamapps/common/GarrysMod' `
  --destination _work/staging/gmod-forwardplus `
  --proxy _work/build/forwardplus/shaderapidx9.dll `
  --runtime D:/OpenRemix/_work/build/forwardplus/src/native/remix.dll `
  --sdl D:/OpenRemix/_work/build/forwardplus/subprojects/SDL3-3.4.14/SDL3_shared.dll
```

This layout supports the minimal native runtime without OpenUSD; builds with
additional runtime dependencies need those DLLs supplied separately. It uses the
installed engine and loose scripts, so it does not establish a pristine Steam
baseline. Addons, native Lua modules, Workshop content and user configuration
directories are excluded from copying.
The stage chooses the stock Kleiner player/hands on spawn, so account preferences
for unavailable Workshop models cannot add missing-model geometry to the capture.
This fixture does not change the account's model preference.

Launch from the staged directory with `REMIX_CONFIG_DIR` set to its
`runtime-config`, `OPENREMIX_PROXY_CONTENT_ROOT` set to the stage,
`OPENREMIX_PROXY_BACKEND=forwardplus`, and `OPENREMIX_PROXY_MODS=0`:

```powershell
./gmod_win64.exe -game garrysmod -insecure -noaddons -noworkshop -novid `
  -windowed -w 960 -h 540 -condebug -conclearlog +sv_lan 1 +map gm_construct
```

The proxy diagnostics report backend selection and bake progress. Optional
`OPENREMIX_PROXY_LOG_ENGINE_ERRORS=1` also records engine startup error messages
before forwarding them to the engine's normal error handler.


## Standalone BSP diagnostics (Windows)

`replay.width = 1920` and `replay.height = 1080` select a fixed client/render
resolution; each dimension accepts integers from 64 through 4096. The default
is 640 x 360. The camera aspect ratio follows the requested dimensions. The
host fails if Windows cannot provide the requested client size; GPU timing
reports also record the actual render and output sizes for verification.
The diagnostic window permits sizes larger than the desktop for 4K captures.

Optional replay settings `replay.measureFrames = 240` and
`replay.warmupFrames = 600` collect a bounded steady-state GPU timing window.
`replay.warmupSeconds = 5` also requires five seconds of warmup (default: three).
The host waits for the selected backend, requested denoiser/TAA, complete materials
and (for Forward+) an active bake. `replay.waitForBake = 0` permits explicit
unbaked experiments. Warmup accepts distinct completed GPU frames; duplicate or
unavailable results are ignored. Backend, bake-hash and resolution changes restart
the window. Both the frame and elapsed-time warmup requirements must be met;
actual warmup can exceed the requested frame count. Warmup must be at least 16
frames, both counts are capped at 10000, and warmup time is bounded to 0–60 seconds.
Measurement mode removes the normal replay's artificial pause between frames.

The output basename receives `.timings.csv` raw samples and `.timings.json` mean,
median, nearest-rank p95, resolution, bake identity and per-pass summaries. Pass
inclusive/self times can overlap through nesting; do not sum the pass rows into
a frame total. The full-frame GPU duration is reported separately. Reports include
host observation timestamps; GPU query results trail execution by several frames.
These are GPU timings for the static replay, not game FPS or a quality-matched
comparison between rendering techniques. The host exits after the window and a
short screenshot drain; the command-line frame limit remains a timeout bound.
Incomplete runs fail without emitting a successful timing summary. Use a fresh
output basename for each run and preserve its settings alongside the reports.

The `bsp-replay` target reads installed Source content through the same BSP,
VPK, VMT and VTF loaders as the proxy, without replacing any game DLLs. It
opens a preview (640 x 360 by default) at the map's first player spawn, selects Forward+,
publishes complete static bake inputs, prints bake progress, and captures a
frame near the end of the requested frame budget. An optional final settings
file accepts `key = value` lines (and `#` comments):

```powershell
bsp-replay.exe runtime/remix.dll "E:/SteamLibrary/steamapps/common/GarrysMod" gm_flatgrass output.png 1800
```

The runtime's SDL DLL must be on `PATH`. Use an isolated `REMIX_CONFIG_DIR`
(or OpenRemix's `tests/isolated_config.py`) to preserve your normal profiles.
The content root is read-only; `OPENREMIX_PROXY_CONTENT_ROOT` supplies that
same override when embedding the loader in another host. Search paths are
initialized once per process.

This is a static map diagnostic, with static VMT shading and textures. Engine
material proxies, styled lights, dynamic entities, particles, game UI and the
engine's sky rendering are absent. Animated textures remain unresolved. Missing
assets and incomplete static props keep baking pending; direct lighting can
still render. A successful screenshot alone does not establish that baking
finished: check the logged bake state (`3`, active `1`). Increase the frame
budget when a map is still baking. Source face fans that fold in lightmap space
retain their geometry and use probes instead of a surface chart.


Static props preserve both alpha testing and ordinary alpha blending. An
alpha-tested material may also set `$translucent`; texture alpha still selects
cutout coverage, following Source's `EvaluateBlendRequirements`. Constant or
vertex alpha can independently require blending. Additive effects and unsupported
material operations remain with the engine and keep complete baking pending.
The native transport samples source-over sheets as thin coverage, including
vertex-alpha terrain layers. BSP water opts into smooth optical bake boundaries:
constant refraction/absorption is captured while normal-map ripples stay live.
This does not add primary water rendering or refractive caustics to Forward+.
For load-time bake experiments, a replay settings file can raise
`rtx.native.forward.bakeBatch` to `1024`; retain isolated configuration so this
experiment does not become a game preference.


Replay settings can select a reproducible inspection view in Source world units:

```ini
replay.position = -1792 -1536 -80
replay.angles = 0 0 0
replay.fov = 65
rtx.native.forward.bakeBatch = 1024
```

Positions override the eye directly; angles are Source pitch/yaw/roll in degrees.
Set `replay.mods = path/to/mods` to exercise the runtime's shared USD material
replacements, including generated PBR packs. Relative paths resolve beside the
settings file; the runtime must be built with USD support. The replay preserves
the original upload's legacy material key even though its decoded textures have
temporary SDK IDs. The loader's normal mod ordering and lazy texture loading apply.
If `REMIX_CONFIG_DIR` is set, a completed bake also produces an adjacent
`output.probes.json` containing the actual published capture origins, projection
boxes and influence boxes. The runtime's `probes.request` diagnostic generates
this file without GPU readback. Reports and screenshots must be newly written;
existing output files cannot establish a successful replay.

The BSP's playable `env_cubemap` sample positions seed geometry-constrained
reflection fitting through the optional version-4 bake descriptor. Solid origins
are refused, duplicate fits are merged, and remaining capacity uses automatic
placement from empty BSP leaves, with hardware checks against the complete
static geometry. This keeps room placement independent of a coarsened irradiance
grid and avoids capturing from inside props.
Hints retain authored order; `rtx.native.forward.reflectionProbes` remains the
capture budget (maximum 16), so a low budget cannot cover every room.


For an automatic-placement experiment, launch the replay with
`OPENREMIX_PROXY_REFLECTION_HINTS=0`. The BSP loader then supplies no authored
cubemap sites; the native fitter derives candidates from empty leaves, validates
them with hardware rays, and uses the same boundary/detail constraints. The
published report identifies the resulting captures. Four probes remain the default
budget; maps with many rooms may need `rtx.native.forward.reflectionProbes = 16`.
This toggle is a diagnostic option and does not edit the map or installed game.
