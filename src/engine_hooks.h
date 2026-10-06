#pragma once

// Engine-side hooks that l4d2-rtx's client mod provided for L4D2: GMod has no
// such mod, so the proxy patches the engine itself.
namespace proxy::engine {

// Installs the hooks once engine.dll and materialsystem.dll are loaded (call
// repeatedly; returns whether they are in place).
bool install();

} // namespace proxy::engine
