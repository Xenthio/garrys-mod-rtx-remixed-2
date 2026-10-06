#pragma once

#include <cstdio>

// Engine-side skipping of static rendering the BSP world replaces (opaque
// world brush batches, depth pre-fill, displacement batches, static props,
// overlays): code patches and vtable hooks in engine.dll, no cvars (nothing a
// server can query). All calls on the main thread.
namespace proxy::engine_patches {

// Locates and validates the targets in the loaded engine.dll; false (and no
// patching ever) when anything does not match this build.
bool install();

// Before the engine's DrawWorldLists runs for a view: skips the static
// content when the BSP world provides it for that view.
void beginWorldLists();
// After it returned: restores the engine's code.
void endWorldLists();

void dumpStatistics(FILE *out);

} // namespace proxy::engine_patches
