// Half-Life 2 RTX: the engine side, none yet. GMod's engine_hooks and the x86
// engine side of L4D2 and Portal 2 (x86_engine.cpp) tell the proxy about the
// frame (the world lists and overlay markers, the map for the BSP world,
// forced cvars, anti-culling, flashlights); until this build has them, every
// draw reaches the proxy as the shader API makes it (the world from the
// engine's own draws, the BSP world idle) and nothing in engine.dll,
// client.dll or server.dll is patched.
#include "engine_hooks.h"

namespace proxy::engine {

bool install() { return false; }

} // namespace proxy::engine
