#pragma once

#include "pe_module.h"

#include <windows.h>

#include <cstdio>

// The x86 engine side (game.h PROXY_X86_ENGINE: Left 4 Dead 2 and Portal 2,
// one engine branch): what the GMod build does with its engine_* modules,
// done in process from engine.dll and client.dll (it was the job of
// l4d2-rtx's client mod in "OpenRemix mode"). One implementation, each game's
// patterns, offsets and slots beside each other where they are used.
//
//  * x86_engine.cpp: install (lazily, from Present, until engine.dll,
//    client.dll and materialsystem.dll are loaded), the world-lists and
//    overlay markers, the map that is loaded (polled), cvar forcing, the
//    engine-side static prop skip.
//  * x86_cull.cpp: anti-culling code patches in engine.dll and client.dll.
//  * bridge_ui.cpp: the runtime's own UI on F12 (remix_ui; shared with
//    Half-Life 2 RTX).
//  * L4D2 only (l4d2.h): the survivors' flashlights, the common infected.
//
// Every code address is found by a byte pattern (first match in the module's
// code, as l4d2-rtx did) and checked against the bytes it expects before it is
// patched; what does not match is logged (shaderapi_proxy_diag.log) and left
// out, the statistics file says what is in place.
namespace proxy::x86 {

// The module helpers (pe_module.h), as this side has always named them.
using pe::Module;
using pe::Pattern;
using pe::bytesAt;
using pe::rttiVtable;
using pe::createInterface;
using pe::swapSlot;
using pe::writeCode;
using pe::writeJump;
using pe::findNetProp;
using pe::envFlag;
using pe::envFloat;

// "openremix:..." markers through the calling thread's material system render
// context (queued with the draws under mat_queue_mode 2).
void marker(const char *text);
// client.dll's current view id (VIEW_MAIN 0, VIEW_3DSKY 1, VIEW_MONITOR 2,
// ...; Portal 2's portal views have ids of their own, above the engine's),
// -1 when unknown.
constexpr int kViewMain = 0, kView3dSky = 1, kViewMonitor = 2;
int viewId();
// The game window (remix_ui::install, else found by class) and whether the
// calling thread is the one that owns it: the engine's main thread.
void setGameWindow(HWND window);
bool onMainThread();

// x86_cull.cpp: installs the anti-culling patches that match (engine.dll
// and client.dll loaded); once.
void installCull(const Module &engine, const Module &client);
void dumpCull(FILE *out);

// Everything above (and L4D2's flashlights), for the statistics file.
void dumpEngineStatistics(FILE *out);

} // namespace proxy::x86
