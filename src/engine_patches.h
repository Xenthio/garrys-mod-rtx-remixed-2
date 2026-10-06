#pragma once

#include <cstdio>

// Engine-side skipping of static rendering the BSP world replaces (opaque
// world brush batches, depth pre-fill, displacement batches, static props,
// overlays): code patches and vtable hooks in engine.dll, no cvars (nothing a
// server can query). OPENREMIX_PROXY_ENGINE_SKIP=0 turns it off; the proxy
// drops whatever the engine still draws either way.
namespace proxy::engine_patches {

// Locates and validates the targets in the loaded engine.dll (any thread);
// each one that does not match this build stays off. False when disabled.
bool install();

// Main thread, before the engine's DrawWorldLists runs for a view: skips the
// static content when the BSP world provides it.
void beginWorldLists();
// After it returned: restores the engine's code.
void endWorldLists();

void dumpStatistics(FILE *out);

} // namespace proxy::engine_patches
