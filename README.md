# gmodrtx

A proxy `shaderapidx9.dll` for Garry's Mod's x86-64 branch that wraps the
Source shader API (and later D3D9) to submit the game's rendering to
OpenRemix through the Remix C API, in process (no 32-bit bridge). Ported from
the L4D2 proxy in l4d2-rtx (`proxy/`).

## Status

Phase 1: forwarding proxy. Loads the original as `shaderapidx9_orig.dll`,
wraps `ShaderDeviceMgr001`, `ShaderApi030`, `ShaderDevice001`,
`ShaderShadow010`, `DebugTextureInfo001`, `MaterialSystemHardwareConfig012`,
`VBAllocTracker001` and the material system's `VShaderUtil001`
(`RestoreShaderObjects`, slot 12), with per-slot call statistics.

## x64 notes

- Wrapper thunks swap RCX (`this`) and jump through the real vtable; typed
  hooks are plain functions taking the wrapper first.
- GMod keeps the classic five `IAppSystem` methods (sourcesdk-minimal), so
  `IShaderDeviceMgr::SetMode` and `IShaderUtil::RestoreShaderObjects` are slot 12.
- `tools/rtti_vtables64.py <dll>` lists vtables from x64 MSVC RTTI.

## Build

    meson setup --cross-file cross/x86_64-w64-mingw32.txt _build
    meson compile -C _build

Deploy: rename `bin/win64/shaderapidx9.dll` to `shaderapidx9_orig.dll` and copy
`_build/shaderapidx9.dll` in its place.

## Dev environment

`dev/` holds scripts for a reflink copy of the install with its own Proton
prefix (`/home/cr/proj/gmod-dev`): `run.sh [+map gm_construct]`, `stop.sh`,
and `con.sh "cmd"`. This engine branch has no `-netconport`: `con.sh` writes
`garrysmod/data/devcmd.txt`, which `dev/lua/devcmd.lua` (installed as
`garrysmod/lua/autorun/client/devcmd.lua`) runs as console commands
(`lua <code>` runs client Lua). Screenshots: `con.sh screenshot`
(`garrysmod/screenshots/*.tga`).
