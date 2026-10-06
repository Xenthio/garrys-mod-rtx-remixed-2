#pragma once

#include <cstdio>

// Lights read where the engine keeps them, independent of what is drawn or
// in view: lightstyle values (switchable/flickering map lights), active
// dlights, projected textures (lamps, flashlights). Interfaces only: no code
// patches, nothing a server can query.
namespace proxy::engine_lights {

// Resolves the engine interfaces and installs the shadow manager hooks
// (main thread, engine.dll loaded). False when unavailable.
bool install();

// Once per main view on the main thread (the DrawWorldLists hook): samples
// the engine's light state for the renderer.
void sample();

void dumpStatistics(FILE *out);

} // namespace proxy::engine_lights
