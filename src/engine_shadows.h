#pragma once

#include <cstdio>

// The engine's entity shadows (render-to-texture and blobby: a caster drawn
// into the shadow render target, then multiply-blended decals on what it
// falls on) switched off at the shadow manager, since the path tracer casts
// the shadows. Vtable hooks on IShadowMgr only: no cvars (nothing a server
// can query or override), no code patches. Projected textures (flashlights,
// lamps) use the same manager and are left alone.
// OPENREMIX_PROXY_ENGINE_SHADOWS=0 keeps the engine's shadows.
namespace proxy::engine_shadows {

// Validates the shadow manager of the loaded engine.dll against the one build
// this was written for and installs the hooks (main thread). All or nothing:
// false, with nothing hooked, when disabled or when anything differs.
bool install();

void dumpStatistics(FILE *out);

} // namespace proxy::engine_shadows
