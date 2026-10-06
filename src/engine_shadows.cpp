#include "engine_shadows.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// How the engine's entity shadows are stopped (all in CShadowMgr, engine.dll,
// reached by client.dll through VEngineShadowMgr002):
//
//  * The client decides per shadow, per update (CClientShadowMgr::UpdateShadow):
//    a shadow whose ShadowInfo_t::m_FalloffBias is 255 ("fully faded") is
//    disabled with EnableShadow(false) and is not built: no ProjectShadow, no
//    projection into the client's leaves, so it is never found for rendering
//    into the shadow render target either. That is the engine's own path for
//    faded-out entities. So every non-flashlight shadow gets bias 255 when it
//    is created, and SetFalloffBias cannot change it.
//  * Whatever still reaches the engine is emptied there: ProjectShadow runs
//    with a leaf count of 0 (it still removes the shadow from the surfaces
//    and leaves it was on and stores its matrices, but projects into
//    nothing: no decals on the world), and AddShadowToBrushModel /
//    AddShadowToModel return at once (no decals on brush entities, props and
//    models).
//
// A shadow is told from a projected texture by the engine's own flag
// (Shadow_t::m_Flags & SHADOW_FLASHLIGHT), read from the element GetInfo
// returns: nothing is tracked here, so shadows made before the hooks, handle
// reuse and level changes need no care. Flashlights pass through every hook
// untouched.
//
// Everything below is for one engine build. In install() the vtable and each
// slot used must be at these engine.dll RVAs, and the instructions the layout
// was read from must be these bytes.
namespace proxy::engine_shadows {
namespace {

constexpr uintptr_t kShadowMgrVtable = 0x3cd330;
// IShadowMgr slots (SDK 2013 order up to here; verified by disassembly).
constexpr unsigned kSlotProjectShadow = 3, kSlotGetInfo = 5, kSlotAddShadowToBrushModel = 7,
                   kSlotAddShadowToModel = 10, kSlotSetFalloffBias = 15, kSlotCreateShadowEx = 19;
constexpr uintptr_t kProjectShadow = 0x14cbb0, kGetInfo = 0x14b760, kAddShadowToBrushModel = 0x146540,
                    kAddShadowToModel = 0x1466e0, kSetFalloffBias = 0x14f6d0, kCreateShadowEx = 0x149a30;

// Shadow_t: 0x160 bytes, ShadowInfo_t first (m_FalloffBias at 0x5c), m_Flags
// (unsigned short) at 0x88 with SHADOW_FLASHLIGHT = 2, SHADOW_DISABLED = 4.
constexpr size_t kFlagsOffset = 0x88;
constexpr uint8_t kFlashlightFlag = 2; // also the CreateShadowEx creation flag
constexpr uint8_t kTransparentBias = 255;
constexpr uint16_t kInvalidHandle = 0xffff;

struct Expected {
  uintptr_t rva;
  const char *bytes; // hex, space separated
  const char *what;
};
constexpr Expected kExpected[] = {
    // GetInfo: movzx eax,dx; imul rax,rax,0x160; add rax,[rcx+0x10]; ret
    {0x14b760, "0F B7 C2 48 69 C0 60 01 00 00 48 03 41 10 C3", "GetInfo"},
    // SetFalloffBias: ...; mov [rdx+rax+0x5c],r8b; ret
    {0x14f6d0, "0F B7 C2 48 69 D0 60 01 00 00 48 8B 41 10 44 88 44 02 5C C3", "SetFalloffBias"},
    // ProjectShadow: mov rax,rsp; sub rsp,0xa8 (so the 7th argument,
    // nLeafCount, is [rsp+0xe0] inside and [rsp+0x38] on entry)
    {0x14cbb0, "48 8B C4 48 81 EC A8 00 00 00", "ProjectShadow frame"},
    // imul rdi,r14,0x160; add rdi,[rbp+0x10]; test byte [rdi+0x88],4; jne end
    {0x14cc3e, "49 69 FE 60 01 00 00 48 03 7D 10 F6 87 88 00 00 00 04 0F 85", "ProjectShadow flags"},
    // cmp [rsp+0xe0],ebx (0); je end: no leaves, nothing projected
    {0x14ce6f, "39 9C 24 E0 00 00 00 0F 84", "ProjectShadow leaf count"},
    // CreateShadowEx: mov eax,[rsp+0x80] (5th argument: creation flags)
    {0x149bb1, "8B 84 24 80 00 00 00", "CreateShadowEx flags argument"},
    // mov [r14+0x88],ax
    {0x149be2, "66 41 89 86 88 00 00 00", "CreateShadowEx flags member"},
    // test al,2; je (not a flashlight)
    {0x149c0c, "A8 02 0F 84", "CreateShadowEx flashlight flag"},
    // CreateShadow (slot 0): mov r10,[rcx]; jmp [r10+0x98] (slot 19)
    {0x149a20, "4C 8B 11 49 FF A2 98 00 00 00", "CreateShadow forwards to CreateShadowEx"},
    // AddShadowToBrushModel: movzx r14d,byte [r15+rbx+0x88]; shr r14b,1; and r14b,1
    {0x1465cb, "45 0F B6 B4 1F 88 00 00 00 41 D0 EE 41 80 E6 01", "AddShadowToBrushModel flashlight flag"},
};

uintptr_t g_engine = 0, g_engineSize = 0;
std::atomic<bool> g_installed{false};
std::atomic<const char *> g_status{"not installed"};
// The mismatch, when there is one: an Expected::what or a slot name.
std::atomic<const char *> g_detail{""};

using GetInfoFn = uint8_t *(*)(void *self, uint16_t handle);
using SetFalloffBiasFn = uintptr_t (*)(void *self, uint16_t handle, uint8_t bias);
using CreateShadowExFn = uintptr_t (*)(void *self, void *material, void *modelMaterial, void *bindProxy,
                                       uintptr_t creationFlags);
using AddShadowToBrushModelFn = uintptr_t (*)(void *self, uint16_t handle, void *model, const void *origin,
                                              const void *angles);
using AddShadowToModelFn = uintptr_t (*)(void *self, uint16_t handle, uint16_t model);

// Set once in install(), before any hook is in a slot.
GetInfoFn g_getInfo = nullptr;
SetFalloffBiasFn g_setFalloffBias = nullptr;
CreateShadowExFn g_createShadowEx = nullptr;
AddShadowToBrushModelFn g_addShadowToBrushModel = nullptr;
AddShadowToModelFn g_addShadowToModel = nullptr;

struct Statistics {
  std::atomic<uint64_t> created{0}, flashlightsCreated{0}, biasForced{0}, projections{0}, brushModels{0}, models{0},
      flashlightCalls{0};
} g_stats;

// An entity shadow, not a projected texture. The client only passes handles
// it got from CreateShadow (the engine indexes with them unchecked).
bool entityShadow(void *self, uint16_t handle) {
  if (handle == kInvalidHandle)
    return false;
  if (g_getInfo(self, handle)[kFlagsOffset] & kFlashlightFlag) {
    g_stats.flashlightCalls.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  return true;
}

// Also what slot 0 (CreateShadow) forwards to.
uintptr_t createShadowEx(void *self, void *material, void *modelMaterial, void *bindProxy, uintptr_t creationFlags) {
  const uintptr_t result = g_createShadowEx(self, material, modelMaterial, bindProxy, creationFlags);
  const uint16_t handle = uint16_t(result);
  if (handle == kInvalidHandle)
    return result;
  if (creationFlags & kFlashlightFlag) {
    g_stats.flashlightsCreated.fetch_add(1, std::memory_order_relaxed);
  } else {
    // Before the client's first update of it.
    g_setFalloffBias(self, handle, kTransparentBias);
    g_stats.created.fetch_add(1, std::memory_order_relaxed);
  }
  return result;
}

uintptr_t setFalloffBias(void *self, uint16_t handle, uint8_t bias) {
  if (entityShadow(self, handle)) {
    if (bias != kTransparentBias)
      g_stats.biasForced.fetch_add(1, std::memory_order_relaxed);
    bias = kTransparentBias;
  }
  return g_setFalloffBias(self, handle, bias);
}

uintptr_t addShadowToBrushModel(void *self, uint16_t handle, void *model, const void *origin, const void *angles) {
  if (entityShadow(self, handle)) {
    g_stats.brushModels.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  return g_addShadowToBrushModel(self, handle, model, origin, angles);
}

uintptr_t addShadowToModel(void *self, uint16_t handle, uint16_t model) {
  if (entityShadow(self, handle)) {
    g_stats.models.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  return g_addShadowToModel(self, handle, model);
}

} // namespace
} // namespace proxy::engine_shadows

// CShadowMgr::ProjectShadow(handle, origin, projectionDir, worldToShadow,
// size, nLeafCount, pLeaves, maxHeight, falloffOffset, falloffAmount,
// casterOrigin): continues in the engine's function with every argument as
// the caller passed it (whatever follows the leaf count included), except
// that an entity shadow's leaf count becomes 0.
extern "C" {
void *openremix_projectShadowOriginal = nullptr;
int openremix_emptyProjection(void *self, unsigned handle) {
  using namespace proxy::engine_shadows;
  if (!entityShadow(self, uint16_t(handle)))
    return 0;
  // The client updated a shadow that should have been skipped as faded (it
  // predates the hooks, or came by a path that does not look at the bias).
  g_setFalloffBias(self, uint16_t(handle), kTransparentBias);
  g_stats.projections.fetch_add(1, std::memory_order_relaxed);
  return 1;
}
void openremix_projectShadow();
}
asm(R"(
  .text
  .globl openremix_projectShadow
openremix_projectShadow:
  .intel_syntax noprefix
  push rbp
  mov rbp, rsp
  sub rsp, 0x20
  mov [rbp+0x10], rcx
  mov [rbp+0x18], rdx
  mov [rbp+0x20], r8
  mov [rbp+0x28], r9
  call openremix_emptyProjection
  mov rcx, [rbp+0x10]
  mov rdx, [rbp+0x18]
  mov r8, [rbp+0x20]
  mov r9, [rbp+0x28]
  test eax, eax
  leave
  jz 1f
  mov dword ptr [rsp+0x38], 0
1:
  jmp [rip+openremix_projectShadowOriginal]
  .att_syntax prefix
)");

namespace proxy::engine_shadows {
namespace {

bool bytesAt(uintptr_t rva, const char *hex) {
  const char *p = hex;
  for (; *p; ++rva) {
    char *end = nullptr;
    const unsigned long byte = std::strtoul(p, &end, 16);
    if (end == p || rva >= g_engineSize || *reinterpret_cast<const uint8_t *>(g_engine + rva) != uint8_t(byte))
      return false;
    for (p = end; *p == ' ';)
      ++p;
  }
  return true;
}

struct Hook {
  unsigned slot;
  uintptr_t rva;
  void *hook;
  const char *name;
};

// One pointer write: safe while another thread calls through the vtable.
bool writeSlot(void **entry, void *value) {
  DWORD old;
  if (!VirtualProtect(entry, sizeof(void *), PAGE_READWRITE, &old))
    return false;
  InterlockedExchangePointer(entry, value);
  VirtualProtect(entry, sizeof(void *), old, &old);
  return true;
}

} // namespace

bool install() {
  static std::atomic<bool> done{false};
  if (done.exchange(true))
    return g_installed;
  char v[8] = {};
  if (GetEnvironmentVariableA("OPENREMIX_PROXY_ENGINE_SHADOWS", v, sizeof(v)) && v[0] == '0') {
    g_status = "disabled (OPENREMIX_PROXY_ENGINE_SHADOWS=0)";
    return false;
  }
  HMODULE engine = GetModuleHandleA("engine.dll");
  using CreateInterfaceFn = void *(*)(const char *name, int *returnCode);
  auto factory = engine ? reinterpret_cast<CreateInterfaceFn>(
                              reinterpret_cast<void *>(GetProcAddress(engine, "CreateInterface")))
                        : nullptr;
  if (!factory) {
    g_status = "no engine.dll";
    return false;
  }
  g_engine = reinterpret_cast<uintptr_t>(engine);
  const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(engine);
  const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(g_engine + dos->e_lfanew);
  g_engineSize = nt->OptionalHeader.SizeOfImage;

  void *manager = factory("VEngineShadowMgr002", nullptr);
  if (!manager) {
    g_status = "no interface";
    return false;
  }
  if (kShadowMgrVtable + 24 * sizeof(void *) > g_engineSize ||
      *static_cast<uintptr_t *>(manager) != g_engine + kShadowMgrVtable) {
    g_status = "vtable mismatch";
    return false;
  }
  void **vtable = *static_cast<void ***>(manager);

  const Hook hooks[] = {
      // The bias first: from then on new shadows never get as far as the rest.
      {kSlotSetFalloffBias, kSetFalloffBias, reinterpret_cast<void *>(&setFalloffBias), "SetFalloffBias"},
      {kSlotCreateShadowEx, kCreateShadowEx, reinterpret_cast<void *>(&createShadowEx), "CreateShadowEx"},
      {kSlotProjectShadow, kProjectShadow, reinterpret_cast<void *>(&openremix_projectShadow), "ProjectShadow"},
      {kSlotAddShadowToBrushModel, kAddShadowToBrushModel, reinterpret_cast<void *>(&addShadowToBrushModel),
       "AddShadowToBrushModel"},
      {kSlotAddShadowToModel, kAddShadowToModel, reinterpret_cast<void *>(&addShadowToModel), "AddShadowToModel"},
  };
  // The slots must still hold the engine's own functions: anything else
  // (another hook) is not what was verified.
  if (reinterpret_cast<uintptr_t>(vtable[kSlotGetInfo]) != g_engine + kGetInfo) {
    g_status = "slot mismatch";
    g_detail = "GetInfo";
    return false;
  }
  for (const Hook &hook : hooks)
    if (reinterpret_cast<uintptr_t>(vtable[hook.slot]) != g_engine + hook.rva) {
      g_status = "slot mismatch";
      g_detail = hook.name;
      return false;
    }
  for (const Expected &expected : kExpected)
    if (!bytesAt(expected.rva, expected.bytes)) {
      g_status = "code mismatch";
      g_detail = expected.what;
      return false;
    }

  g_getInfo = reinterpret_cast<GetInfoFn>(g_engine + kGetInfo);
  g_setFalloffBias = reinterpret_cast<SetFalloffBiasFn>(g_engine + kSetFalloffBias);
  g_createShadowEx = reinterpret_cast<CreateShadowExFn>(g_engine + kCreateShadowEx);
  g_addShadowToBrushModel = reinterpret_cast<AddShadowToBrushModelFn>(g_engine + kAddShadowToBrushModel);
  g_addShadowToModel = reinterpret_cast<AddShadowToModelFn>(g_engine + kAddShadowToModel);
  openremix_projectShadowOriginal = reinterpret_cast<void *>(g_engine + kProjectShadow);

  for (size_t i = 0; i < sizeof(hooks) / sizeof(hooks[0]); ++i) {
    if (writeSlot(&vtable[hooks[i].slot], hooks[i].hook))
      continue;
    // All or nothing: the engine's functions go back.
    bool restored = true;
    for (size_t k = 0; k < i; ++k)
      restored &= writeSlot(&vtable[hooks[k].slot], reinterpret_cast<void *>(g_engine + hooks[k].rva));
    g_status = restored ? "hook failed (nothing installed)" : "hook failed (partly installed)";
    g_detail = hooks[i].name;
    return false;
  }
  g_status = "ok (hooked)";
  g_installed = true;
  return true;
}

void dumpStatistics(FILE *out) {
  const char *detail = g_detail.load();
  fprintf(out, "engine shadows: %s%s%s\n", g_status.load(), *detail ? ": " : "", detail);
  if (!g_installed)
    return;
  static ULONGLONG last = GetTickCount64();
  const ULONGLONG now = GetTickCount64();
  const double seconds = now > last ? double(now - last) / 1000 : 1;
  last = now;
  fprintf(out,
          "   per second: entity shadows created faded %.1f, bias changes overridden %.1f, projections emptied %.1f, "
          "receivers dropped: brush models %.1f, models %.1f | projected textures: created %.1f, calls passed "
          "%.1f\n",
          double(g_stats.created.exchange(0)) / seconds, double(g_stats.biasForced.exchange(0)) / seconds,
          double(g_stats.projections.exchange(0)) / seconds, double(g_stats.brushModels.exchange(0)) / seconds,
          double(g_stats.models.exchange(0)) / seconds, double(g_stats.flashlightsCreated.exchange(0)) / seconds,
          double(g_stats.flashlightCalls.exchange(0)) / seconds);
}

} // namespace proxy::engine_shadows
