#include "engine_hooks.h"
#include "engine_lights.h"
#include "engine_mirrors.h"
#include "engine_nocull.h"
#include "engine_patches.h"
#include "engine_shadows.h"
#include "game.h"
#include "frame_profile.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace proxy::engine {
namespace {

using CreateInterfaceFn = void *(*)(const char *name, int *returnCode);

void *createInterface(const char *module, const char *name) {
  HMODULE m = GetModuleHandleA(module);
  auto factory = m ? reinterpret_cast<CreateInterfaceFn>(GetProcAddress(m, "CreateInterface")) : nullptr;
  return factory ? factory(name, nullptr) : nullptr;
}

void *patch(void **vtable, unsigned slot, void *hook) {
  DWORD old;
  VirtualProtect(&vtable[slot], sizeof(void *), PAGE_READWRITE, &old);
  void *previous = vtable[slot];
  vtable[slot] = hook;
  VirtualProtect(&vtable[slot], sizeof(void *), old, &old);
  return previous;
}

// Markers reach the shader API (SetPIXMarker, slot 216) through the render
// context, so they stay ordered with the draws when the material system
// queues rendering to its own thread (mat_queue_mode). The material system's
// slots per game: game::kSlotGetRenderContext, kSlotContextSetPIXMarker.
void *g_materials = nullptr;
// IVEngineClient (VEngineClient013) slot 51: GetLevelName ("maps/x.bsp"), in
// GMod's and TF2's engine.dll alike.
void *g_engineClient = nullptr;
char g_level[256] = {};

DWORD g_mainThread = 0; // the thread of the first Present: the engine's main one

using WorldListClock = std::chrono::steady_clock;
thread_local WorldListClock::time_point worldListsStarted;
thread_local uint32_t worldPatchMicroseconds = 0;

bool profileWorldLists() {
  // The engine hook can run before the renderer has loaded remix.dll.
  static const bool enabled = GetEnvironmentVariableW(L"OPENREMIX_PROXY_FRAME_PROFILE", nullptr, 0) != 0;
  return enabled;
}

uint32_t microseconds(WorldListClock::duration duration) {
  return uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(duration).count());
}

// Texture detail Very High (mat_picmip -1) with the Advanced video options
// hidden: the command line cannot carry it (the engine drops a negative value
// after a "+" command). Run once the configs and "+" commands have been, from
// the main thread (the command buffer is not shared with the render thread).
// OPENREMIX_PROXY_PICMIP: another value, or "keep" to leave it as saved.
void applyTextureDetail() {
  if (!game::kGMod) // with GMod's Advanced video options (main.cpp) only
    return;
  static unsigned presents = 0;
  static bool done = false;
  if (done || ++presents < 60 || GetCurrentThreadId() != g_mainThread || !g_engineClient)
    return;
  done = true;
  char value[16] = "-1";
  if (GetEnvironmentVariableA("OPENREMIX_PROXY_PICMIP", value, sizeof(value)) && std::strcmp(value, "keep") == 0)
    return;
  char command[48];
  std::snprintf(command, sizeof(command), "mat_picmip %d\n", std::atoi(value));
  // IVEngineClient013 slot 106: ClientCmd_Unrestricted.
  using ClientCmdFn = void (*)(void *, const char *);
  reinterpret_cast<ClientCmdFn>((*static_cast<void ***>(g_engineClient))[106])(g_engineClient, command);
}
} // namespace
void marker(const char *text) {
  if (!g_materials)
    return;
  using GetRenderContextFn = void *(*)(void *);
  using SetPIXMarkerFn = void (*)(void *, unsigned long, const char *);
  void *context = reinterpret_cast<GetRenderContextFn>(
      (*static_cast<void ***>(g_materials))[game::kSlotGetRenderContext])(g_materials);
  if (!context)
    return;
  reinterpret_cast<SetPIXMarkerFn>((*static_cast<void ***>(context))[game::kSlotContextSetPIXMarker])(context, 0, text);
  // TF2's GetRenderContext adds a reference (IRefCounted::Release, slot 1).
  using ReleaseFn = int (*)(void *);
  if (game::kRenderContextAddRef)
    reinterpret_cast<ReleaseFn>((*static_cast<void ***>(context))[1])(context);
}
// Announces the map ("openremix:map <maps/x.bsp>", loads the BSP world) the
// first time the world is drawn after it changed.
void noteLevel() {
  if (!g_engineClient)
    return;
  using GetLevelNameFn = const char *(*)(void *);
  const char *level = reinterpret_cast<GetLevelNameFn>((*static_cast<void ***>(g_engineClient))[51])(g_engineClient);
  if (!level || !*level || std::strncmp(level, g_level, sizeof(g_level)) == 0)
    return;
  std::snprintf(g_level, sizeof(g_level), "%s", level);
  char text[300];
  std::snprintf(text, sizeof(text), "openremix:map %s", g_level);
  marker(text);
}
} // namespace proxy::engine

// IVRenderView (VEngineRenderView014) slot 13 (GMod, TF2): DrawWorldLists. Inside it the
// engine draws the world surfaces and, with DRAWWORLDLISTS_DRAW_SKYBOX, the
// 2D skybox (dynamic-buffer draws). The wrapper preserves every argument
// register (RCX, RDX, R8, R9, XMM0-3) and copies eight stack arguments, so it
// forwards whatever GMod's signature is (a typed 3-argument hook crashed: the
// engine then bound a null material).
extern "C" {
void *openremix_drawWorldListsOriginal = nullptr;
void openremix_worldListsBegin() {
  using namespace proxy::engine;
  const bool profile = profileWorldLists();
  if (profile) worldListsStarted = WorldListClock::now();
  proxy::engine::noteLevel();
  proxy::engine_lights::sample();
  const auto patchStarted = profile ? WorldListClock::now() : WorldListClock::time_point{};
  proxy::engine_patches::beginWorldLists();
  if (profile) worldPatchMicroseconds = microseconds(WorldListClock::now() - patchStarted);
  proxy::engine::marker("openremix:world_lists 1");
}
void openremix_worldListsEnd() {
  using namespace proxy::engine;
  const bool profile = profileWorldLists();
  proxy::engine::marker("openremix:world_lists 0");
  const auto patchStarted = profile ? WorldListClock::now() : WorldListClock::time_point{};
  proxy::engine_patches::endWorldLists();
  if (profile) {
    const auto now = WorldListClock::now();
    proxy::frame_profile::recordWorldLists(microseconds(now - worldListsStarted),
        worldPatchMicroseconds + microseconds(now - patchStarted));
  }
}
void openremix_drawWorldLists();
}
asm(R"(
  .text
  .globl openremix_drawWorldLists
openremix_drawWorldLists:
  .intel_syntax noprefix
  push rbp
  mov rbp, rsp
  sub rsp, 0xA0
  mov [rbp+0x10], rcx
  mov [rbp+0x18], rdx
  mov [rbp+0x20], r8
  mov [rbp+0x28], r9
  movdqu [rsp+0x60], xmm0
  movdqu [rsp+0x70], xmm1
  movdqu [rsp+0x80], xmm2
  movdqu [rsp+0x90], xmm3
  call openremix_worldListsBegin
  mov rax, [rbp+0x30]
  mov [rsp+0x20], rax
  mov rax, [rbp+0x38]
  mov [rsp+0x28], rax
  mov rax, [rbp+0x40]
  mov [rsp+0x30], rax
  mov rax, [rbp+0x48]
  mov [rsp+0x38], rax
  mov rax, [rbp+0x50]
  mov [rsp+0x40], rax
  mov rax, [rbp+0x58]
  mov [rsp+0x48], rax
  mov rax, [rbp+0x60]
  mov [rsp+0x50], rax
  mov rax, [rbp+0x68]
  mov [rsp+0x58], rax
  mov rcx, [rbp+0x10]
  mov rdx, [rbp+0x18]
  mov r8, [rbp+0x20]
  mov r9, [rbp+0x28]
  movdqu xmm0, [rsp+0x60]
  movdqu xmm1, [rsp+0x70]
  movdqu xmm2, [rsp+0x80]
  movdqu xmm3, [rsp+0x90]
  call [rip+openremix_drawWorldListsOriginal]
  mov [rsp+0x60], rax
  movdqu [rsp+0x70], xmm0
  call openremix_worldListsEnd
  mov rax, [rsp+0x60]
  movdqu xmm0, [rsp+0x70]
  leave
  ret
  .att_syntax prefix
)");

namespace proxy::engine {
namespace {
} // namespace

bool install() {
  static bool done = false;
  if (!g_mainThread)
    g_mainThread = GetCurrentThreadId();
  if (done) {
    applyTextureDetail();
    engine_mirrors::install();
    return true;
  }
  // OPENREMIX_PROXY_ENGINE_HOOKS=0 leaves the engine untouched.
  static const bool disabled = [] {
    char v[8] = {};
    return GetEnvironmentVariableA("OPENREMIX_PROXY_ENGINE_HOOKS", v, sizeof(v)) && v[0] == '0';
  }();
  if (disabled)
    return false;
  void *renderView = createInterface("engine.dll", "VEngineRenderView014");
  g_materials = createInterface("materialsystem.dll", game::kMaterialSystemInterface);
  g_engineClient = createInterface("engine.dll", "VEngineClient013");
  if (!renderView || !g_materials)
    return false;
  openremix_drawWorldListsOriginal =
      patch(*static_cast<void ***>(renderView), 13, reinterpret_cast<void *>(&openremix_drawWorldLists));
  engine_patches::install();
  engine_nocull::install(g_materials, renderView);
  engine_lights::install();
  engine_shadows::install();
  engine_mirrors::install();
  done = true;
  return true;
}

} // namespace proxy::engine
