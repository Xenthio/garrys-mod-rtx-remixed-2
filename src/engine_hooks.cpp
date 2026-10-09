#include "engine_hooks.h"
#include "engine_lights.h"
#include "engine_nocull.h"
#include "engine_patches.h"
#include "engine_shadows.h"

#include <windows.h>

#include <cstdio>
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
// queues rendering to its own thread (mat_queue_mode). Slots verified in
// GMod's materialsystem.dll: IMaterialSystem::GetRenderContext 98 (no AddRef),
// IMatRenderContext::SetPIXMarker 140 (jumps to IShaderAPI slot 216).
void *g_materials = nullptr;
// IVEngineClient (VEngineClient013) slot 51: GetLevelName ("maps/x.bsp").
void *g_engineClient = nullptr;
char g_level[256] = {};
} // namespace
void marker(const char *text) {
  if (!g_materials)
    return;
  using GetRenderContextFn = void *(*)(void *);
  using SetPIXMarkerFn = void (*)(void *, unsigned long, const char *);
  void *context = reinterpret_cast<GetRenderContextFn>((*static_cast<void ***>(g_materials))[98])(g_materials);
  if (context)
    reinterpret_cast<SetPIXMarkerFn>((*static_cast<void ***>(context))[140])(context, 0, text);
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

// IVRenderView (VEngineRenderView014) slot 13: DrawWorldLists. Inside it the
// engine draws the world surfaces and, with DRAWWORLDLISTS_DRAW_SKYBOX, the
// 2D skybox (dynamic-buffer draws). The wrapper preserves every argument
// register (RCX, RDX, R8, R9, XMM0-3) and copies eight stack arguments, so it
// forwards whatever GMod's signature is (a typed 3-argument hook crashed: the
// engine then bound a null material).
extern "C" {
void *openremix_drawWorldListsOriginal = nullptr;
void openremix_worldListsBegin() {
  proxy::engine::noteLevel();
  proxy::engine_lights::sample();
  proxy::engine_patches::beginWorldLists();
  proxy::engine::marker("openremix:world_lists 1");
}
void openremix_worldListsEnd() {
  proxy::engine::marker("openremix:world_lists 0");
  proxy::engine_patches::endWorldLists();
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
  if (done)
    return true;
  // OPENREMIX_PROXY_ENGINE_HOOKS=0 leaves the engine untouched.
  static const bool disabled = [] {
    char v[8] = {};
    return GetEnvironmentVariableA("OPENREMIX_PROXY_ENGINE_HOOKS", v, sizeof(v)) && v[0] == '0';
  }();
  if (disabled)
    return false;
  void *renderView = createInterface("engine.dll", "VEngineRenderView014");
  g_materials = createInterface("materialsystem.dll", "VMaterialSystem080");
  g_engineClient = createInterface("engine.dll", "VEngineClient013");
  if (!renderView || !g_materials)
    return false;
  openremix_drawWorldListsOriginal =
      patch(*static_cast<void ***>(renderView), 13, reinterpret_cast<void *>(&openremix_drawWorldLists));
  engine_patches::install();
  engine_nocull::install(g_materials, renderView);
  engine_lights::install();
  engine_shadows::install();
  done = true;
  return true;
}

} // namespace proxy::engine
