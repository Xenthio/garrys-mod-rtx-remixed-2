#pragma once

#include <cstdio>

// The engine's refraction view for reflective glass (func_reflective_glass)
// skipped: the proxy draws the glass as a path-traced mirror
// (bsp::mirrorMaterial), so the scene the client renders behind it went
// unused. (The mirrored view stays: the local player's model is drawn only
// there.) One code patch in client.dll of one build, checked at install. Off
// with OPENREMIX_PROXY_MIRRORS=0 (the glass as the engine draws it) or
// OPENREMIX_PROXY_GLASS_VIEWS=1 (the view kept).
namespace proxy::engine_mirrors {

// Main thread; again until client.dll is loaded, then once.
void install();

void dumpStatistics(FILE *out);

} // namespace proxy::engine_mirrors
