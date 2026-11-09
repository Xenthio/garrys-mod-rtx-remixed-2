#pragma once

// Engine-side hooks that l4d2-rtx's client mod provided for L4D2: GMod has no
// such mod, so the proxy patches the engine itself. TF2 (x64) shares
// engine_hooks.cpp's interface hooks (the world-list and map markers); GMod's
// build-specific engine patches (engine_patches, engine_nocull, engine_lights,
// engine_shadows) are off there (gmod_only_stubs.cpp).
namespace proxy::engine {

// Installs the hooks once engine.dll and materialsystem.dll are loaded (call
// repeatedly; returns whether they are in place).
bool install();

} // namespace proxy::engine
