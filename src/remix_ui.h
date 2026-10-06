#pragma once

#include <windows.h>

#include <remix/remix_c.h>

#include <cstdio>

namespace proxy::remix_ui {

// F12 toggles OpenRemix's developer panel. Remix runs in process on the game
// window (SDL wraps it), but nothing pumps SDL's queue for a borrowed window
// and the runtime leaves the hotkey to its host: the game window is
// subclassed here, and while the panel is open its input goes to the
// panel's SDL queue instead of the game (as l4d2-rtx + bridge do).
// Call on the thread that ran remixapi Startup (it owns the window's events).
void install(HWND window, remixapi_Interface *api);

// Restores the window procedure (before the runtime releases its window).
void uninstall();

// Whether the panel is open.
bool active();

void dumpStatistics(FILE *out);

} // namespace proxy::remix_ui
