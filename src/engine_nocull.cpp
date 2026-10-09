#include "engine_nocull.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

// How this build decides what to draw, and what is changed (engine.dll and
// client.dll of one build; every address below is an RVA checked at install):
//
//  * World: R_BuildWorldLists (engine) calls R_SetupAreaBits and then walks
//    the BSP (RecursiveWorldNode, 0xf6fe0 / 0xf7210). A node is skipped when
//    its visframe is not the current one (PVS: untouched) and when
//    R_CullNode (0x135ea0) says so: the node's area is not in
//    g_RenderAreaBits (untouched), or its box is outside the frustum (the
//    view's, or the area's portal-narrowed one), tested by the function
//    R_CullNode ends in (0x135f20). That function is made to return "not
//    culled". (r_frustumcullworld 0 is alive but skips R_CullNode altogether,
//    the area test with it, and does nothing for entities.)
//  * Area portals: R_SetupAreaBits (0x136500) flows from the view's area
//    through the portals (R_FlowThroughArea, 0x136140). A portal is passed
//    when it is open (cl.m_chAreaPortalBits), the server says the area behind
//    is connected (cl.m_chAreaBits), the view origin is in front of its plane
//    and its screen rectangle intersects the rectangle the flow came through.
//    The last two depend on where the camera is and looks: they are taken out
//    (the plane test never skips, the portal rectangle is always the full
//    screen), and "do not flow back into an area on the stack" becomes "do
//    not flow into an area already marked visible", so the flow is a plain
//    flood fill (the rectangles no longer differ, and without it the number
//    of paths through a well connected map would explode). Closed portals
//    and the server's area bits decide as before.
//  * Entities: CClientLeafSystem::BuildRenderablesList (client 0x210800)
//    collects the renderables of the leaves the world walk listed
//    (CollateRenderablesInLeaf, 0x210b90), and drops each one whose box
//    fails engine->DoesBoxTouchAreaFrustum (r_PortalTestEnts, the default;
//    IVEngineClient slot 58) or engine->CullBox (slot 33), and props and
//    models that engine->IsOccluded (slot 69, func_occluder brushes seen
//    from the camera). While BuildRenderablesList runs for such a view, on
//    that thread, the three answer "visible". Everything else that asks the
//    engine (beams, temporary entities, the shadow list) is answered as
//    before.
//
// Which views: those that render to the back buffer (the render context's
// current render target is null: what the proxy takes world views from) and
// are not shadow depth. The code patches are in place only while
// IVRenderView::BuildWorldLists (slot 12) runs for such a view: the main
// thread is the only one that runs that code. R_GetVisibleFogVolume, which
// uses the same functions to find water in view, keeps the frustum, so the
// engine's water passes are chosen as before.
namespace proxy::engine_nocull {
namespace {

struct Module {
  uintptr_t base = 0, size = 0;
  bool open(const char *name) {
    auto module = reinterpret_cast<uintptr_t>(GetModuleHandleA(name));
    if (!module)
      return false;
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
      return false;
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(module + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
      return false;
    base = module;
    size = nt->OptionalHeader.SizeOfImage;
    return true;
  }
  bool has(uintptr_t rva, size_t n) const { return base && rva <= size && n <= size - rva; }
  bool holds(const void *p, size_t n) const {
    const auto address = reinterpret_cast<uintptr_t>(p);
    return base && address >= base && has(address - base, n);
  }
  template <typename T> T *at(uintptr_t rva) const { return reinterpret_cast<T *>(base + rva); }
};
Module g_engine, g_client, g_materialSystem;

// "48 8B C3": hex bytes, space separated.
bool bytesAt(const Module &module, uintptr_t rva, const char *bytes) {
  for (const char *p = bytes; *p; ++rva) {
    char *end;
    const unsigned long value = std::strtoul(p, &end, 16);
    if (end == p || !module.has(rva, 1) || *module.at<uint8_t>(rva) != uint8_t(value))
      return false;
    for (p = end; *p == ' '; ++p) {
    }
  }
  return true;
}

struct Expected {
  uintptr_t rva;
  const char *bytes;
  const char *what;
};

// The first mismatch's description, or null.
template <size_t N> const char *mismatch(const Module &module, const Expected (&list)[N]) {
  for (const Expected &expected : list)
    if (!bytesAt(module, expected.rva, expected.bytes))
      return expected.what;
  return nullptr;
}

// ---- engine.dll ----

constexpr uintptr_t kRenderViewVtable = 0x3cdcc0, kBuildWorldLists = 0x152780;
constexpr unsigned kSlotBuildWorldLists = 12; // IVRenderView
constexpr uintptr_t kEngineClientVtable = 0x3a3078, kCullBox = 0x7d300, kDoesBoxTouchAreaFrustum = 0x7e410,
                    kIsOccluded = 0x7f590;
constexpr unsigned kSlotCullBox = 33, kSlotDoesBoxTouchAreaFrustum = 58, kSlotIsOccluded = 69; // IVEngineClient

constexpr Expected kBuildWorldListsCode[] = {
    // mov rcx,[g_EngineRenderer]; movzx eax,byte [rsp+0x30] (bShadowDepth, the
    // 6th argument); mov [rsp+0x30],al; mov r10,[rcx]; jmp [r10+0x38]
    {0x152780, "48 8B 0D 71 16 38 00 0F B6 44 24 30 88 44 24 30 4C 8B 11 49 FF 62 38", "BuildWorldLists thunk"},
    // R_BuildWorldLists: mov ecx,edi; call R_SetupAreaBits
    {0xf5ac2, "8B CF E8 37 0A 04 00", "R_BuildWorldLists calls R_SetupAreaBits"},
};
constexpr Expected kCullBoxCode[] = {
    // mov rax,r8; mov rcx,rdx; mov rdx,rax; lea r8,[g_Frustum]; jmp R_CullBox
    {0x7d300, "49 8B C0 48 8B CA 48 8B D0 4C 8D 05 B0 07 76 00 E9 AB 2E 22 00", "CullBox"},
};
constexpr Expected kAreaFrustumCode[] = {
    // mov ecx,r9d (area); call GetAreaFrustum; ...; call R_CullBox; test al,al; sete al
    {0x7e410,
     "48 89 5C 24 08 57 48 83 EC 20 41 8B C9 49 8B D8 48 8B FA E8 68 6D 0B 00 4C 8B C0 48 8B D3 48 8B CF E8 8A 1D 22 "
     "00 48 8B 5C 24 30 84 C0 0F 94 C0 48 83 C4 20 5F C3",
     "DoesBoxTouchAreaFrustum"},
};
constexpr Expected kIsOccludedCode[] = {
    // call OcclusionSystem(); ...; jmp [r9+0x10] (IOcclusionSystem::IsOccluded)
    {0x7f590,
     "48 89 5C 24 08 57 48 83 EC 20 49 8B D8 48 8B FA E8 DB 6D 0A 00 4C 8B C3 48 8B D7 48 8B C8 4C 8B 08 48 8B 5C 24 "
     "30 48 83 C4 20 5F 49 FF 61 10",
     "IsOccluded"},
};
constexpr Expected kWorldFrustumCode[] = {
    // R_CullNode(frustum, node, &clipMask): rex push rbx; cmp byte
    // [g_bViewerInSolidSpace],0; ...; jne frustum; movsx r10,word [rdx+0x10]
    // (node->area); test r10w,r10w; jle frustum
    {0x135ea0, "40 53 80 3D 3F EF 6A 00 00 4D 8B D8 48 8B DA 75 56 4C 0F BF 52 10 66 45 85 D2 7E 4B", "R_CullNode"},
    // lea rcx,[g_RenderAreaBits]; test [rax+rcx],r8b; jne +4; mov al,1; pop rbx; ret
    {0x135ed2, "48 8D 0D 37 E3 6A 00 44 84 04 08 75 04 B0 01 5B C3", "R_CullNode area test"},
    // mov r8,rcx; mov rdx,r11; mov rcx,rbx; pop rbx; jmp +0xa (the frustum test)
    {0x135f07, "4C 8B C1 49 8B D3 48 8B CB 5B E9 0A 00 00 00", "R_CullNode tail"},
    // mov r9d,[rdx]; xorps xmm3,xmm3; mov eax,r9d; and eax,0x80000000
    {0x135f20, "44 8B 0A 0F 57 DB 41 8B C1 25 00 00 00 80", "frustum test"},
    // RecursiveWorldNode, both variants: lea rcx,[g_Frustum]; call R_CullNode; test al,al; jne
    {0xf7063, "48 8D 0D 56 6A 6E 00 E8 31 EE 03 00 84 C0 0F 85", "RecursiveWorldNode calls R_CullNode"},
    {0xf7290, "48 8D 0D 29 68 6E 00 E8 04 EC 03 00 84 C0 0F 85", "RecursiveWorldNode (other) calls R_CullNode"},
};
constexpr Expected kAreaFlowCode[] = {
    {0x136140, "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 54 41 55 41 56 41 57 48 81 EC 90 03 00 00",
     "R_FlowThroughArea"},
    // movzx eax,byte [r15+rcx+g_RenderAreaBits]; bts eax,r13d; mov [...],al
    {0x136255, "41 0F B6 84 0F 10 42 7E 00 44 0F AB E8 41 88 84 0F 10 42 7E 00", "area marked visible"},
    // the same for g_AreaStack
    {0x13626a, "41 0F B6 84 0F B0 45 7E 00 44 0F AB E8 41 88 84 0F B0 45 7E 00", "area put on the stack"},
    // test [r10+rax+g_AreaStack],r9b; jne next
    {0x13630f, "45 84 8C 02 B0 45 7E 00 0F 85", "area on the stack test"},
    // test [r8+rax+cl.m_chAreaPortalBits],dl; je next
    {0x13633b, "41 84 94 00 39 2E 67 00 0F 84", "portal open test"},
    // comiss xmm2,xmm7 (-0.1); jb next
    {0x136385, "0F 2F D7 0F 82 04 01 00 00", "portal side test"},
    // test [r10+rax+cl.m_chAreaBits],r9b; je next
    {0x136395, "45 84 8C 02 19 2E 67 00 0F 84", "area connected test"},
    // comiss xmm2,xmm0 (tolerance); jbe full rectangle; mov r9,r15 (GetPortalScreenExtents follows)
    {0x1363b2, "0F 2F D0 76 1F 4D 8B CF", "portal extents branch"},
    // movaps xmm0,[full rectangle]; movups [rsp+0x30],xmm0
    {0x1363d6, "0F 28 05 33 69 29 00 0F 11 44 24 30", "full rectangle"},
    {0x3ccd10, "00 00 80 BF 00 00 80 3F 00 00 80 3F 00 00 80 BF", "full rectangle constant"},
    {0x13648d, "E8 AE FC FF FF", "recursion"},
    // btr of the area's g_AreaStack bit on the way out
    {0x1364c8, "48 8D 0D 31 9B EC FF 41 0F B6 84 0F B0 45 7E 00", "area taken off the stack"},
    // R_SetupAreaBits: movups [g_RenderAreaBits],xmm0 (both halves); calls the flow
    {0x1365a3, "0F 11 05 66 DC 6A 00 0F 11 05 6F DC 6A 00", "R_SetupAreaBits clears the visible areas"},
    {0x136abb, "E8 80 F6 FF FF", "R_SetupAreaBits calls R_FlowThroughArea"},
};

// Engine code rewritten while BuildWorldLists runs (main thread).
struct CodePatch {
  const char *name;
  uintptr_t rva;
  size_t size;
  uint8_t original[6], patched[6];
  uint8_t *at = nullptr;
  bool usable = false, applied = false;
  std::atomic<const char *> state{"not installed"};
};
// mov r9d,[rdx] -> xor al,al; ret
CodePatch g_worldFrustum{"WorldFrustum", 0x135f20, 3, {0x44, 0x8B, 0x0A}, {0x32, 0xC0, 0xC3}};
// The three of R_FlowThroughArea go together.
CodePatch g_areaFlow[] = {
    // g_AreaStack (0x7e45b0) -> g_RenderAreaBits (0x7e4210) in the test's displacement
    {"AreaFlowVisited", 0x136313, 2, {0xB0, 0x45}, {0x10, 0x42}},
    // jb next -> 6-byte nop
    {"AreaFlowSide", 0x136388, 6, {0x0F, 0x82, 0x04, 0x01, 0x00, 0x00}, {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00}},
    // jbe full rectangle -> jmp
    {"AreaFlowExtents", 0x1363b5, 1, {0x76}, {0xEB}},
};

void arm(CodePatch &patch) {
  DWORD old;
  uint8_t *at = g_engine.at<uint8_t>(patch.rva);
  if (!g_engine.has(patch.rva, patch.size) || std::memcmp(at, patch.original, patch.size) != 0) {
    patch.state = "bytes differ";
  } else if (!VirtualProtect(at, patch.size, PAGE_EXECUTE_READWRITE, &old)) {
    patch.state = "cannot make writable";
  } else {
    patch.at = at;
    patch.usable = true;
    patch.state = "ok";
  }
}

// False when something else rewrote the bytes: they are theirs now.
bool flip(CodePatch &patch, bool on) {
  if (!patch.usable)
    return false;
  if (patch.applied == on)
    return true;
  if (std::memcmp(patch.at, on ? patch.original : patch.patched, patch.size) != 0) {
    patch.usable = patch.applied = false;
    patch.state = "bytes changed at runtime";
    return false;
  }
  std::memcpy(patch.at, on ? patch.patched : patch.original, patch.size);
  FlushInstructionCache(GetCurrentProcess(), patch.at, patch.size);
  patch.applied = on;
  return true;
}

void flipAreaFlow(bool on) {
  for (CodePatch &patch : g_areaFlow)
    if (!patch.usable)
      return;
  bool intact = true;
  for (CodePatch &patch : g_areaFlow)
    intact &= flip(patch, on);
  if (intact)
    return;
  // One of them is gone: the others go back to the engine's bytes for good.
  for (CodePatch &patch : g_areaFlow) {
    flip(patch, false);
    if (patch.usable) {
      patch.usable = false;
      patch.state = "off (another AreaFlow patch failed)";
    }
  }
}

struct VtableHook {
  const char *name;
  const char *module;
  void **slot = nullptr;
  uintptr_t slotRva = 0;
  std::atomic<const char *> state{"not installed"};
};
VtableHook g_buildWorldListsHook{"BuildWorldLists", "engine"}, g_cullBoxHook{"CullBox", "engine"},
    g_areaFrustumHook{"DoesBoxTouchAreaFrustum", "engine"}, g_isOccludedHook{"IsOccluded", "engine"},
    g_buildRenderablesHook{"BuildRenderablesList", "client"};

// One pointer-sized write: callers on other threads see either function.
bool swapSlot(void **slot, void *hook) {
  DWORD old;
  if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old))
    return false;
  InterlockedExchangePointer(slot, hook);
  VirtualProtect(slot, sizeof(void *), old, &old);
  return true;
}

// The slot must hold the build's own function (not another hook), with the
// code it was read from.
template <size_t N>
bool hookSlot(VtableHook &hook, const Module &module, void **vtable, unsigned slot, uintptr_t function,
              const Expected (&code)[N], void *replacement) {
  if (!module.holds(&vtable[slot], sizeof(void *)) || reinterpret_cast<uintptr_t>(vtable[slot]) != module.base + function) {
    hook.state = "slot mismatch";
    return false;
  }
  if (const char *what = mismatch(module, code)) {
    hook.state = what; // which code differs
    return false;
  }
  hook.slot = &vtable[slot];
  hook.slotRva = reinterpret_cast<uintptr_t>(hook.slot) - module.base;
  if (!swapSlot(hook.slot, replacement)) {
    hook.state = "cannot write vtable";
    return false;
  }
  hook.state = "ok";
  return true;
}

std::atomic<bool> g_ready{false};
std::atomic<const char *> g_status{"not installed"};

// ---- which views ----

// IMaterialSystem::GetRenderContext (slot 98, the calling thread's context, as
// engine_hooks.cpp uses it) and IMatRenderContext::GetRenderTarget (slot 5):
// the top of the context's render target stack, null for the back buffer.
// One function for the immediate and the queued context, a plain read; any
// other code in the slot switches everything off.
constexpr unsigned kSlotGetRenderContext = 98, kSlotGetRenderTarget = 5;
// cmp dword [rcx+0x78],0; jle null; mov eax,[rcx+0x78]; dec eax; imul
// rdx,rax,0x38; mov rax,[rcx+0x68]; mov rax,[rdx+rax]; ret; xor eax,eax; ret
constexpr char kGetRenderTargetCode[] =
    "83 79 78 00 7E 12 8B 41 78 FF C8 48 6B D0 38 48 8B 41 68 48 8B 04 02 C3 33 C0 C3";
void *g_materials = nullptr;
std::atomic<void *> g_getRenderTarget{nullptr};
std::atomic<bool> g_targetBroken{false};
std::atomic<const char *> g_targetState{"not used yet"};

bool backBufferView() {
  if (!g_materials || g_targetBroken.load(std::memory_order_relaxed))
    return false;
  using GetRenderContextFn = void *(*)(void *);
  using GetRenderTargetFn = void *(*)(void *);
  void *context =
      reinterpret_cast<GetRenderContextFn>((*static_cast<void ***>(g_materials))[kSlotGetRenderContext])(g_materials);
  if (!context)
    return false;
  void *function = (*static_cast<void ***>(context))[kSlotGetRenderTarget];
  if (function != g_getRenderTarget.load(std::memory_order_relaxed)) {
    if (!g_materialSystem.holds(function, 1) ||
        !bytesAt(g_materialSystem, reinterpret_cast<uintptr_t>(function) - g_materialSystem.base,
                 kGetRenderTargetCode)) {
      g_targetBroken = true;
      g_targetState = "GetRenderTarget is not the expected code: everything off";
      return false;
    }
    g_getRenderTarget = function;
    g_targetState = "ok";
  }
  return reinterpret_cast<GetRenderTargetFn>(function)(context) == nullptr;
}

// ---- statistics ----

struct Statistics {
  std::atomic<uint64_t> worldKept{0}, worldShadow{0}, worldOffTarget{0};
  std::atomic<uint64_t> buildsKept{0}, buildsShadow{0}, buildsOffTarget{0};
  std::atomic<uint64_t> keptFrustum{0}, keptPortal{0}, keptOccluded{0}, refused{0}, cappedViews{0};
  std::atomic<uint64_t> leaves{0}, staticProps{0}, opaque{0}, translucent{0}, other{0};
} g_stats;

// ---- the client's renderable culling ----

using CullBoxFn = bool (*)(void *self, const float *mins, const float *maxs);
using DoesBoxTouchAreaFrustumFn = bool (*)(void *self, const float *mins, const float *maxs, int area);
using BuildRenderablesListFn = void (*)(void *self, const uint8_t *info);
// Set before the hooks are in their slots.
CullBoxFn g_cullBox = nullptr, g_isOccluded = nullptr;
DoesBoxTouchAreaFrustumFn g_doesBoxTouchAreaFrustum = nullptr;
BuildRenderablesListFn g_buildRenderablesList = nullptr;

// The thread inside BuildRenderablesList for a view that keeps everything (0:
// none), and that view's counts (touched by that thread only).
std::atomic<DWORD> g_buildThread{0};
struct ViewCounts {
  uint32_t frustum, portal, occluded, refused;
  bool offScreen; // the box last asked about was kept although out of view
} g_view;
uint32_t g_cap = 4096;

bool building() {
  const DWORD thread = g_buildThread.load(std::memory_order_relaxed);
  return thread && thread == GetCurrentThreadId();
}

// Whether one more renderable the engine would cull may stay.
bool keep(uint32_t &count) {
  if (g_view.frustum + g_view.portal + g_view.occluded >= g_cap) {
    ++g_view.refused;
    return false;
  }
  ++count;
  return true;
}

bool cullBox(void *self, const float *mins, const float *maxs) {
  const bool culled = g_cullBox(self, mins, maxs);
  if (!building())
    return culled;
  g_view.offScreen = culled && keep(g_view.frustum);
  return culled && !g_view.offScreen;
}

bool doesBoxTouchAreaFrustum(void *self, const float *mins, const float *maxs, int area) {
  const bool touches = g_doesBoxTouchAreaFrustum(self, mins, maxs, area);
  if (!building())
    return touches;
  g_view.offScreen = !touches && keep(g_view.portal);
  return touches || g_view.offScreen;
}

// Occluders hide what is behind them as seen from the camera: light still
// gets there. A box kept out of view was never meant to reach the occlusion
// test, so it is not asked about.
bool isOccluded(void *self, const float *mins, const float *maxs) {
  if (!building())
    return g_isOccluded(self, mins, maxs);
  if (g_view.offScreen) {
    g_view.offScreen = false;
    return false;
  }
  return g_isOccluded(self, mins, maxs) && !keep(g_view.occluded);
}

// SetupRenderInfo_t: m_pWorldListInfo 0x00 (m_LeafCount at +4), m_pRenderList
// 0x08, flags 0x34 (1 detail objects, 2 translucent objects: clear only for
// shadow depth). CClientRenderablesList: 14 groups of 0x4000 entries, their
// counts at 0x380010. Group 6 and 7 get one placeholder per leaf.
constexpr size_t kInfoFlags = 0x34, kListCounts = 0x380010;
constexpr uint8_t kTranslucentObjects = 2;
constexpr int kGroups = 14;

void buildRenderablesList(void *self, const uint8_t *info) {
  const bool shadowDepth = !(info[kInfoFlags] & kTranslucentObjects);
  if (shadowDepth || !backBufferView()) {
    ++(shadowDepth ? g_stats.buildsShadow : g_stats.buildsOffTarget);
    g_buildThread.store(0, std::memory_order_relaxed);
    g_buildRenderablesList(self, info);
    return;
  }
  g_view = {};
  g_buildThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
  g_buildRenderablesList(self, info);
  g_buildThread.store(0, std::memory_order_relaxed);

  ++g_stats.buildsKept;
  g_stats.keptFrustum += g_view.frustum;
  g_stats.keptPortal += g_view.portal;
  g_stats.keptOccluded += g_view.occluded;
  g_stats.refused += g_view.refused;
  g_stats.cappedViews += g_view.refused != 0;
  const uint8_t *world, *list;
  std::memcpy(&world, info, sizeof(world));
  std::memcpy(&list, info + 8, sizeof(list));
  if (!world || !list)
    return;
  int32_t leaves, counts[kGroups];
  std::memcpy(&leaves, world + 4, sizeof(leaves));
  std::memcpy(counts, list + kListCounts, sizeof(counts));
  const int64_t statics = int64_t(counts[0]) + counts[2] + counts[4] + counts[6] - leaves;
  const int64_t opaque = int64_t(counts[1]) + counts[3] + counts[5] + counts[7] - leaves;
  g_stats.leaves += uint64_t(leaves > 0 ? leaves : 0);
  g_stats.staticProps += uint64_t(statics > 0 ? statics : 0);
  g_stats.opaque += uint64_t(opaque > 0 ? opaque : 0);
  g_stats.translucent += uint64_t(counts[8] > 0 ? counts[8] : 0);
  for (int group = 9; group < kGroups; ++group)
    g_stats.other += uint64_t(counts[group] > 0 ? counts[group] : 0);
}

// ---- client.dll ----

constexpr uintptr_t kLeafSystemVtable = 0x6c4708, kBuildRenderablesList = 0x210800, kClientEnginePointer = 0x952418;
constexpr unsigned kSlotBuildRenderablesList = 15; // CClientLeafSystem
constexpr Expected kBuildRenderablesCode[] = {
    {0x210800, "48 8B C4 48 83 EC 58 48 89 70 20 48 8B F2 48 89", "BuildRenderablesList"},
    // movsxd r14,[rax+4] (m_pWorldListInfo->m_LeafCount)
    {0x21086a, "4C 63 70 04", "leaf count"},
    {0x21089b, "E8 F0 02 00 00", "BuildRenderablesList calls CollateRenderablesInLeaf"},
    // test byte [rsi+0x34],2
    {0x2108ab, "F6 46 34 02", "translucent objects flag"},
    {0x210b90, "40 55 41 54 41 55 41 56 41 57 48 81 EC 90 00 00 00", "CollateRenderablesInLeaf"},
    {0x210d8c, "F6 45 34 02", "translucent objects flag (leaf)"},
    // mov rax,[rcx]; call [rax+0x1d0]; test al,al; je next
    {0x210e09, "48 8B 01 FF 90 D0 01 00 00 84 C0 0F 84", "DoesBoxTouchAreaFrustum call"},
    // mov rcx,[engine]
    {0x210e1c, "48 8B 0D F5 15 74 00", "engine pointer"},
    // mov rax,[rcx]; call [rax+0x108]; test al,al; jne next
    {0x210e2d, "48 8B 01 FF 90 08 01 00 00 84 C0 0F 85", "CullBox call"},
    // mov rax,[rcx]; call [rax+0x228]; test al,al; jne next
    {0x210e55, "48 8B 01 FF 90 28 02 00 00 84 C0 0F 85", "IsOccluded call"},
    // movsxd rax,[rdx+0x380010] (the group's count)
    {0x210f00, "48 63 82 10 00 38 00", "render list counts"},
};

// Main thread, once client.dll is there.
void installClient() {
  VtableHook &hook = g_buildRenderablesHook;
  if (!g_client.open("client.dll")) {
    hook.state = "client.dll not readable";
    return;
  }
  using CreateInterfaceFn = void *(*)(const char *name, int *returnCode);
  auto factory = reinterpret_cast<CreateInterfaceFn>(
      reinterpret_cast<void *>(GetProcAddress(reinterpret_cast<HMODULE>(g_client.base), "CreateInterface")));
  void *leafSystem = factory ? factory("ClientLeafSystem002", nullptr) : nullptr;
  if (!leafSystem || !g_client.holds(leafSystem, sizeof(void *)) ||
      !g_client.has(kLeafSystemVtable, (kSlotBuildRenderablesList + 1) * sizeof(void *)) ||
      *static_cast<uintptr_t *>(leafSystem) != g_client.base + kLeafSystemVtable) {
    hook.state = "vtable mismatch";
    return;
  }
  // The client must ask the engine object whose vtable carries the hooks.
  if (!g_client.has(kClientEnginePointer, sizeof(void *)) || !*g_client.at<void **>(kClientEnginePointer) ||
      **g_client.at<uintptr_t *>(kClientEnginePointer) != g_engine.base + kEngineClientVtable) {
    hook.state = "the client's engine pointer is not the engine's own";
    return;
  }
  g_buildRenderablesList = reinterpret_cast<BuildRenderablesListFn>(g_client.base + kBuildRenderablesList);
  hookSlot(hook, g_client, g_client.at<void *>(kLeafSystemVtable), kSlotBuildRenderablesList, kBuildRenderablesList,
           kBuildRenderablesCode, reinterpret_cast<void *>(&buildRenderablesList));
}

} // namespace
} // namespace proxy::engine_nocull

// IVRenderView::BuildWorldLists(list, info, forceViewLeaf, visData,
// bShadowDepth, reflectionWaterHeight): forwarded with every argument
// register and eight stack arguments untouched, as engine_hooks.cpp does for
// DrawWorldLists.
extern "C" {
void *openremix_buildWorldListsOriginal = nullptr;
void openremix_buildWorldListsBegin(int shadowDepth) {
  using namespace proxy::engine_nocull;
  if (!g_ready.load(std::memory_order_acquire))
    return;
  static bool clientAttempted = false;
  if (!clientAttempted && GetModuleHandleA("client.dll")) {
    clientAttempted = true;
    installClient();
  }
  // Left set only when the engine left BuildRenderablesList without returning.
  g_buildThread.store(0, std::memory_order_relaxed);
  const bool keep = !(shadowDepth & 0xff) && backBufferView();
  ++((shadowDepth & 0xff) ? g_stats.worldShadow : keep ? g_stats.worldKept : g_stats.worldOffTarget);
  flip(g_worldFrustum, keep);
  flipAreaFlow(keep);
}
// Stateless, like engine_patches: should the engine ever leave BuildWorldLists
// without returning, the next begin or end puts the code right.
void openremix_buildWorldListsEnd() {
  using namespace proxy::engine_nocull;
  if (!g_ready.load(std::memory_order_acquire))
    return;
  flip(g_worldFrustum, false);
  flipAreaFlow(false);
}
void openremix_buildWorldLists();
}
asm(R"(
  .text
  .globl openremix_buildWorldLists
openremix_buildWorldLists:
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
  movzx ecx, byte ptr [rbp+0x38]
  call openremix_buildWorldListsBegin
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
  call [rip+openremix_buildWorldListsOriginal]
  mov [rsp+0x60], rax
  movdqu [rsp+0x70], xmm0
  call openremix_buildWorldListsEnd
  mov rax, [rsp+0x60]
  movdqu xmm0, [rsp+0x70]
  leave
  ret
  .att_syntax prefix
)");

namespace proxy::engine_nocull {

bool install(void *materials, void *renderView) {
  static std::atomic<bool> attempted{false};
  if (attempted.exchange(true))
    return g_ready;
  char v[16] = {};
  if (GetEnvironmentVariableA("OPENREMIX_PROXY_ENGINE_NOCULL", v, sizeof(v)) && v[0] == '0') {
    g_status = "disabled (OPENREMIX_PROXY_ENGINE_NOCULL=0)";
    return false;
  }
  if (GetEnvironmentVariableA("OPENREMIX_PROXY_ENGINE_NOCULL_MAX", v, sizeof(v)) && v[0] >= '0' && v[0] <= '9')
    g_cap = uint32_t(std::strtoul(v, nullptr, 10));
  if (!g_engine.open("engine.dll") || !g_materialSystem.open("materialsystem.dll")) {
    g_status = "engine.dll or materialsystem.dll not readable";
    return false;
  }
  g_materials = materials;

  // Armed only: the bytes change while BuildWorldLists runs.
  if (const char *what = mismatch(g_engine, kWorldFrustumCode))
    g_worldFrustum.state = what;
  else
    arm(g_worldFrustum);
  if (const char *what = mismatch(g_engine, kAreaFlowCode)) {
    for (CodePatch &patch : g_areaFlow)
      patch.state = what;
  } else {
    bool all = true;
    for (CodePatch &patch : g_areaFlow) {
      arm(patch);
      all &= patch.usable;
    }
    for (CodePatch &patch : g_areaFlow)
      if (!all && patch.usable) {
        patch.usable = false;
        patch.state = "off (another AreaFlow patch failed)";
      }
  }

  using CreateInterfaceFn = void *(*)(const char *name, int *returnCode);
  auto factory = reinterpret_cast<CreateInterfaceFn>(
      reinterpret_cast<void *>(GetProcAddress(reinterpret_cast<HMODULE>(g_engine.base), "CreateInterface")));
  void *client = factory ? factory("VEngineClient013", nullptr) : nullptr;
  if (!client || !g_engine.has(kEngineClientVtable, (kSlotIsOccluded + 1) * sizeof(void *)) ||
      *static_cast<uintptr_t *>(client) != g_engine.base + kEngineClientVtable) {
    g_cullBoxHook.state = g_areaFrustumHook.state = g_isOccludedHook.state = "vtable mismatch";
  } else {
    // Passive until the client's BuildRenderablesList hook names a thread.
    void **vtable = g_engine.at<void *>(kEngineClientVtable);
    g_cullBox = reinterpret_cast<CullBoxFn>(g_engine.base + kCullBox);
    g_doesBoxTouchAreaFrustum = reinterpret_cast<DoesBoxTouchAreaFrustumFn>(g_engine.base + kDoesBoxTouchAreaFrustum);
    g_isOccluded = reinterpret_cast<CullBoxFn>(g_engine.base + kIsOccluded);
    hookSlot(g_cullBoxHook, g_engine, vtable, kSlotCullBox, kCullBox, kCullBoxCode, reinterpret_cast<void *>(&cullBox));
    hookSlot(g_areaFrustumHook, g_engine, vtable, kSlotDoesBoxTouchAreaFrustum, kDoesBoxTouchAreaFrustum,
             kAreaFrustumCode, reinterpret_cast<void *>(&doesBoxTouchAreaFrustum));
    hookSlot(g_isOccludedHook, g_engine, vtable, kSlotIsOccluded, kIsOccluded, kIsOccludedCode,
             reinterpret_cast<void *>(&isOccluded));
  }

  g_ready.store(true, std::memory_order_release);
  if (!renderView || !g_engine.has(kRenderViewVtable, (kSlotBuildWorldLists + 1) * sizeof(void *)) ||
      *static_cast<uintptr_t *>(renderView) != g_engine.base + kRenderViewVtable) {
    g_buildWorldListsHook.state = "vtable mismatch";
  } else {
    openremix_buildWorldListsOriginal = g_engine.at<void>(kBuildWorldLists);
    hookSlot(g_buildWorldListsHook, g_engine, g_engine.at<void *>(kRenderViewVtable), kSlotBuildWorldLists,
             kBuildWorldLists, kBuildWorldListsCode, reinterpret_cast<void *>(&openremix_buildWorldLists));
  }
  g_status = "installed";
  return true;
}

void dumpStatistics(FILE *out) {
  fprintf(out, "== Engine no-cull: %s", g_status.load());
  if (!g_ready.load(std::memory_order_acquire)) {
    fprintf(out, "\n");
    return;
  }
  fprintf(out, " | back-buffer test: %s | at most %u kept per view\n", g_targetState.load(), g_cap);
  fprintf(out, "   %s: %s (engine+0x%llX)\n", g_worldFrustum.name, g_worldFrustum.state.load(),
          (unsigned long long)g_worldFrustum.rva);
  for (const CodePatch &patch : g_areaFlow)
    fprintf(out, "   %s: %s (engine+0x%llX)\n", patch.name, patch.state.load(), (unsigned long long)patch.rva);
  for (const VtableHook *hook :
       {&g_buildWorldListsHook, &g_cullBoxHook, &g_areaFrustumHook, &g_isOccludedHook, &g_buildRenderablesHook})
    fprintf(out, "   %s: %s (slot at %s+0x%llX)\n", hook->name, hook->state.load(), hook->module,
            (unsigned long long)hook->slotRva);
  static ULONGLONG last = GetTickCount64();
  const ULONGLONG now = GetTickCount64();
  const double seconds = now > last ? double(now - last) / 1000 : 1;
  last = now;
  const uint64_t builds = g_stats.buildsKept.exchange(0);
  const double views = builds ? double(builds) : 1;
  fprintf(out,
          "   per second: world lists built without frustum %.1f, shadow depth %.1f, off target %.1f | renderable "
          "lists without frustum %.1f, shadow depth %.1f, off target %.1f\n",
          double(g_stats.worldKept.exchange(0)) / seconds, double(g_stats.worldShadow.exchange(0)) / seconds,
          double(g_stats.worldOffTarget.exchange(0)) / seconds, double(builds) / seconds,
          double(g_stats.buildsShadow.exchange(0)) / seconds, double(g_stats.buildsOffTarget.exchange(0)) / seconds);
  fprintf(out,
          "   per view without frustum: kept outside the frustum %.1f, outside the portal frustum %.1f, occluded "
          "%.1f, refused (cap) %.1f | lists: leaves %.1f, static props %.1f, opaque entities %.1f, translucent %.1f, "
          "other %.1f | views at the cap per second %.1f\n",
          double(g_stats.keptFrustum.exchange(0)) / views, double(g_stats.keptPortal.exchange(0)) / views,
          double(g_stats.keptOccluded.exchange(0)) / views, double(g_stats.refused.exchange(0)) / views,
          double(g_stats.leaves.exchange(0)) / views, double(g_stats.staticProps.exchange(0)) / views,
          double(g_stats.opaque.exchange(0)) / views, double(g_stats.translucent.exchange(0)) / views,
          double(g_stats.other.exchange(0)) / views, double(g_stats.cappedViews.exchange(0)) / seconds);
}

} // namespace proxy::engine_nocull
