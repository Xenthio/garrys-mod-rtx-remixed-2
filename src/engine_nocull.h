#pragma once

#include <cstdio>

// The engine keeps drawing what is outside the camera's view frustum (the path
// tracer needs off-screen objects for shadows, reflections and bounce light):
// in the views that render to the back buffer (main view, 3D skybox) the world
// walk, the area-portal flow and the client's renderable culling no longer
// test the frustum. What is visible is then the map's PVS and the areas
// connected through open area portals. Shadow-depth and off-target views
// (water, monitors, Lua render targets) cull as before. Code patches and
// vtable hooks in engine.dll and client.dll, no cvars.
// OPENREMIX_PROXY_ENGINE_NOCULL=0 turns it off;
// OPENREMIX_PROXY_ENGINE_NOCULL_MAX=<n> caps the renderables kept per view
// (default 4096).
namespace proxy::engine_nocull {

// Validates the loaded engine.dll against the one build this was written for
// and installs what matches (each item on its own; client.dll follows on the
// main thread once it is loaded). materials: IMaterialSystem, renderView:
// IVRenderView. False when disabled.
bool install(void *materials, void *renderView);

void dumpStatistics(FILE *out);

} // namespace proxy::engine_nocull
