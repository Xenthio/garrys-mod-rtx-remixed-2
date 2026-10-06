#pragma once
#include <windows.h>

#include <cstdio>

// D3D9 interception below the original shader API: patches the original
// module's Direct3DCreate9 import and then vtable entries of the objects it
// creates. Every draw the game issues passes through here.
namespace proxy::d3d {

// Hooks `module`'s import of d3d9!Direct3DCreate9. Call once, before the
// shader API creates its device.
bool install(HMODULE module);

// Whether render target 0 is the back buffer (render thread): the views the
// proxy takes draws from.
bool backBufferBound();

// Writes per-interval draw statistics and resets the interval counters.
void dumpStatistics(FILE *out);

} // namespace proxy::d3d
