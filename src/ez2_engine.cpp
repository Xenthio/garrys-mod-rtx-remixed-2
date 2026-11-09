// Entropy : Zero 2: the engine side.
// Wraps IVRenderView::DrawWorldLists and COverlayMgr::RenderOverlays,
// announces map changes to load the BSP world and its lights,
// forces anti-culling and shadow cvars,
// and implements engine_lights for the flashlight / night vision.

#include "engine_hooks.h"
#include "engine_lights.h"
#include "engine_materials.h"
#include "remix_output.h"
#include "bsp_world.h"
#include "game.h"
#include "diag.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace proxy::engine {

using CreateInterfaceFn = void *(*)(const char *, int *);

void *createInterface(const char *moduleName, const char *interfaceName) {
  HMODULE m = GetModuleHandleA(moduleName);
  if (!m)
    m = LoadLibraryA(moduleName);
  if (!m)
    return nullptr;
  auto factory = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(m, "CreateInterface"));
  return factory ? factory(interfaceName, nullptr) : nullptr;
}

bool swapSlot(void **slot, void *hook, void **original = nullptr) {
  DWORD old;
  if (!VirtualProtect(slot, sizeof(void *), PAGE_EXECUTE_READWRITE, &old))
    return false;
  if (original)
    *original = *slot;
  *slot = hook;
  VirtualProtect(slot, sizeof(void *), old, &old);
  return true;
}

void *g_materials = nullptr;
void *g_engineClient = nullptr;
void *g_renderView = nullptr;
void *g_cvars = nullptr;

char g_level[260] = {};

void executeCommand(const char *command) {
  if (!g_engineClient || !command || !*command)
    return;
  // IVEngineClient slot 106: ClientCmd_Unrestricted(const char *cmd)
  using ClientCmdFn = void(PROXY_THISCALL *)(void *, const char *);
  reinterpret_cast<ClientCmdFn>((*static_cast<void ***>(g_engineClient))[106])(g_engineClient, command);
}

// Markers reach the shader API (SetPIXMarker, slot 215) through the render
// context, so they stay ordered with the draws when the material system
// queues rendering to its own thread (mat_queue_mode).
void marker(const char *text) {
  if (!text || !*text)
    return;
  if (g_materials) {
    using GetRenderContextFn = void *(PROXY_THISCALL *)(void *);
    using SetPIXMarkerFn = void(PROXY_THISCALL *)(void *, unsigned long, const char *);
    using ReleaseFn = int(PROXY_THISCALL *)(void *);
    void *context = reinterpret_cast<GetRenderContextFn>((*static_cast<void ***>(g_materials))[98])(g_materials);
    if (context) {
      void **vtable = *static_cast<void ***>(context);
      reinterpret_cast<SetPIXMarkerFn>(vtable[142])(context, 0, text);
      reinterpret_cast<ReleaseFn>(vtable[1])(context);
      return;
    }
  }
  proxy::remix::onMarker(text);
}

void noteLevel() {
  if (!g_engineClient)
    return;
  // IVEngineClient slot 51: GetLevelName() -> const char *
  using GetLevelNameFn = const char *(PROXY_THISCALL *)(void *);
  const char *level = reinterpret_cast<GetLevelNameFn>((*static_cast<void ***>(g_engineClient))[51])(g_engineClient);
  if (!level)
    return;

  if (std::strcmp(level, g_level) == 0)
    return;

  std::snprintf(g_level, sizeof(g_level), "%s", level);
  char text[320];
  if (g_level[0]) {
    std::snprintf(text, sizeof(text), "openremix:map %s", g_level);
    proxy::diag::stage("EZ2: map changed to %s", g_level);
  } else {
    std::snprintf(text, sizeof(text), "openremix:map");
    proxy::diag::stage("EZ2: map unloaded");
  }
  marker(text);
}

// IVRenderView::DrawWorldLists(IWorldRenderList *pList, unsigned long flags, float waterZThreshold)
// Slot 13, __thiscall, pops 12 bytes.
using DrawWorldListsFn = void(PROXY_THISCALL *)(void *, void *, unsigned long, float);
DrawWorldListsFn g_drawWorldLists = nullptr;

void PROXY_THISCALL drawWorldLists(void *self, void *worldLists, unsigned long flags, float waterZ) {
  noteLevel();
  engine_lights::sample();
  marker("openremix:world_lists 1");
  g_drawWorldLists(self, worldLists, flags, waterZ);
  marker("openremix:world_lists 0");
}

// COverlayMgr::RenderOverlays(int sortGroup)
// Slot 7, __thiscall, pops 4 bytes.
using RenderOverlaysFn = void(PROXY_THISCALL *)(void *, int);
RenderOverlaysFn g_renderOverlays = nullptr;

void PROXY_THISCALL renderOverlays(void *self, int sortGroup) {
  marker("openremix:overlays 1");
  g_renderOverlays(self, sortGroup);
  marker("openremix:overlays 0");
}

struct ForcedCvar {
  const char *name;
  int value;
};

const ForcedCvar kForcedCvars[] = {
    // Anti-culling: prevent engine from culling entities and rooms
    {"r_PortalTestEnts", 0},
    {"r_occlusion", 0},
    {"r_entityclips", 0},
    {"r_portalsopenall", 1},
    {"r_drawentities", 1},
    {"r_drawothermodels", 1},
    {"r_drawopaquerenderables", 1},
    {"r_drawtranslucentrenderables", 1},
    {"r_frustumcullworld", 0},
    // Rendering / shadows
    {"r_3dsky", 0},
    {"r_shadows", 0},
    {"r_shadowrendertotexture", 0},
    {"r_shadowfromworldlights", 0},
    {"fog_override", 1},
    {"fog_enable", 0},
    {"mat_softwarelighting", 0},
    {"mat_softwareskin", 0},
    {"cl_brushfastpath", 0},
    {"cl_tlucfastpath", 0},
};

void forceCvars() {
  if (!g_cvars)
    g_cvars = createInterface("vstdlib.dll", "VEngineCvar004");
  if (!g_cvars)
    return;

  // Slot 14 of VEngineCvar004: ConVar *FindVar(const char *name)
  using FindVarFn = void *(PROXY_THISCALL *)(void *, const char *);
  auto findVar = reinterpret_cast<FindVarFn>((*static_cast<void ***>(g_cvars))[14]);
  using SetIntFn = void(PROXY_THISCALL *)(void *, int);

  for (const auto &fc : kForcedCvars) {
    void *var = findVar(g_cvars, fc.name);
    if (!var)
      continue;
    auto *bytes = static_cast<uint8_t *>(var);
    // Strip FCVAR_DEVELOPMENTONLY (1<<1), FCVAR_HIDDEN (1<<4), FCVAR_CHEAT (1<<14)
    *reinterpret_cast<int *>(bytes + 0x14) &= ~((1 << 1) | (1 << 4) | (1 << 14));
    // IConVar is at bytes + 0x18. Slot 2 is SetValue(int)
    void *iconvar = bytes + 0x18;
    reinterpret_cast<SetIntFn>((*static_cast<void ***>(iconvar))[2])(iconvar, fc.value);
  }
}

void applyCvars() {
  static bool applied = false;
  static int presents = 0;
  if (applied)
    return;
  if (++presents < 60 || !g_engineClient)
    return;

  applied = true;
  forceCvars();
  executeCommand(
      "r_PortalTestEnts 0\n"
      "r_occlusion 0\n"
      "r_entityclips 0\n"
      "r_portalsopenall 1\n"
      "r_drawentities 1\n"
      "r_drawothermodels 1\n"
      "r_frustumcullworld 0\n"
      "r_3dsky 0\n"
      "r_shadows 0\n"
      "r_shadowrendertotexture 0\n"
      "r_shadowfromworldlights 0\n"
      "fog_override 1\n"
      "fog_enable 0\n"
      "mat_softwarelighting 0\n"
      "mat_softwareskin 0\n"
      "cl_brushfastpath 0\n"
      "cl_tlucfastpath 0\n");
  proxy::diag::stage("EZ2: anti-culling and render cvars applied");
}

// subobject: the base-class subobject's offset whose vtable is wanted (its
// complete object locator's offset field); -1: the first found.
void **findRttiVtable(HMODULE module, const char *typeName, int32_t subobject = -1) {
  if (!module || !typeName)
    return nullptr;
  auto *base = reinterpret_cast<const uint8_t *>(module);
  const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    return nullptr;
  const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE)
    return nullptr;

  const size_t typeNameLen = std::strlen(typeName) + 1;
  const uint8_t *namePtr = nullptr;
  const auto *sec = IMAGE_FIRST_SECTION(nt);
  const WORD numSec = nt->FileHeader.NumberOfSections;

  for (WORD s = 0; s < numSec; ++s) {
    const uint8_t *secStart = base + sec[s].VirtualAddress;
    const size_t secSize = sec[s].Misc.VirtualSize;
    for (size_t i = 0; i + typeNameLen <= secSize; ++i) {
      if (std::memcmp(secStart + i, typeName, typeNameLen) == 0) {
        namePtr = secStart + i;
        break;
      }
    }
    if (namePtr)
      break;
  }
  if (!namePtr)
    return nullptr;

  const uintptr_t typeDescVa = reinterpret_cast<uintptr_t>(namePtr) - 8;
  const uint8_t *colPtr = nullptr;

  for (WORD s = 0; s < numSec; ++s) {
    const uint8_t *secStart = base + sec[s].VirtualAddress;
    const size_t secSize = sec[s].Misc.VirtualSize;
    for (size_t i = 0; i + 16 <= secSize; i += 4) {
      if (*reinterpret_cast<const uintptr_t *>(secStart + i + 12) == typeDescVa &&
          *reinterpret_cast<const uint32_t *>(secStart + i) == 0 &&
          (subobject < 0 || *reinterpret_cast<const int32_t *>(secStart + i + 4) == subobject)) {
        colPtr = secStart + i;
        break;
      }
    }
    if (colPtr)
      break;
  }
  if (!colPtr)
    return nullptr;

  const uintptr_t colVa = reinterpret_cast<uintptr_t>(colPtr);
  for (WORD s = 0; s < numSec; ++s) {
    const uint8_t *secStart = base + sec[s].VirtualAddress;
    const size_t secSize = sec[s].Misc.VirtualSize;
    for (size_t i = 0; i + 8 <= secSize; i += 4) {
      if (*reinterpret_cast<const uintptr_t *>(secStart + i) == colVa) {
        return reinterpret_cast<void **>(const_cast<uint8_t *>(secStart + i + 4));
      }
    }
  }
  return nullptr;
}

#ifdef PROXY_GAME_EZ2
// IVEngineClient::CullBox (slot 33, as in the 2013 SDK's cdll_int.h): the
// client's leaf system drops each entity whose box is outside the view
// frustum (CollateRenderablesInLeaf; r_PortalTestEnts 0 asks this instead of
// DoesBoxTouchAreaFrustum), so a prop just off screen cast no shadow. Every
// box is answered "visible": the leaves are the PVS already
// (r_frustumcullworld 0). OPENREMIX_PROXY_ENGINE_NOCULL=0 keeps the engine's.
constexpr unsigned kSlotCullBox = 33;
using CullBoxFn = bool(PROXY_THISCALL *)(void *, const float *, const float *);
CullBoxFn g_cullBox = nullptr;

bool PROXY_THISCALL cullBox(void *, const float *, const float *) { return false; }

void installNoCull() {
  char value[8] = {};
  if (GetEnvironmentVariableA("OPENREMIX_PROXY_ENGINE_NOCULL", value, sizeof(value)) && value[0] == '0')
    return;
  void **vtable = *static_cast<void ***>(g_engineClient);
  if (swapSlot(&vtable[kSlotCullBox], reinterpret_cast<void *>(&cullBox), reinterpret_cast<void **>(&g_cullBox)))
    proxy::diag::stage("EZ2: IVEngineClient::CullBox hooked (entities kept off screen)");
}

// func_areaportalwindow: a brush (halflife/black, a window imposter) that
// fades in over a doorway with the distance from the view, standing in for
// the room behind once the portal closes. The portals are all open
// (r_portalsopenall 1) and the path tracer has the room, but each view
// fades it anew (opaque in some, as far from their origin): a black wall
// across the doorway that came and went. C_FuncAreaPortalWindow's
// IClientRenderable::ShouldDraw (slot 3 of the vtable of its subobject at 4)
// says no.
bool PROXY_THISCALL portalWindowShouldDraw(void *) { return false; }

bool installPortalWindows() {
  HMODULE client = GetModuleHandleA("client.dll");
  if (!client)
    return false;
  void **vtable = findRttiVtable(client, ".?AVC_FuncAreaPortalWindow@@", 4);
  if (vtable && swapSlot(&vtable[3], reinterpret_cast<void *>(&portalWindowShouldDraw)))
    proxy::diag::stage("EZ2: C_FuncAreaPortalWindow::ShouldDraw hooked (portal window brushes hidden)");
  else
    proxy::diag::stage("EZ2: C_FuncAreaPortalWindow's renderable vtable not found");
  return true;
}
#endif

bool install() {
  static bool installed = false;
  if (!installed) {
    g_materials = createInterface("materialsystem.dll", "VMaterialSystem080");

    g_renderView = createInterface("engine.dll", "VEngineRenderView014");
    if (!g_renderView)
      g_renderView = createInterface("engine.dll", "VEngineRenderView013");

    g_engineClient = createInterface("engine.dll", "VEngineClient014");
    if (!g_engineClient)
      g_engineClient = createInterface("engine.dll", "VEngineClient013");

    if (g_renderView && g_engineClient) {
      // Hook DrawWorldLists (slot 13)
      void **renderViewVtable = *static_cast<void ***>(g_renderView);
      swapSlot(&renderViewVtable[13], reinterpret_cast<void *>(&drawWorldLists),
               reinterpret_cast<void **>(&g_drawWorldLists));

      // Hook COverlayMgr::RenderOverlays (slot 7)
      if (HMODULE engineMod = GetModuleHandleA("engine.dll")) {
        if (void **overlayVtable = findRttiVtable(engineMod, ".?AVCOverlayMgr@@")) {
          swapSlot(&overlayVtable[7], reinterpret_cast<void *>(&renderOverlays),
                   reinterpret_cast<void **>(&g_renderOverlays));
          proxy::diag::stage("EZ2: COverlayMgr::RenderOverlays hooked");
        }
      }

      engine_lights::install();
#ifdef PROXY_GAME_EZ2
      installNoCull();
#endif

      installed = true;
      proxy::diag::stage("EZ2: engine hooks installed successfully (DrawWorldLists, ShadowMgr, GetLevelName)");
    }
  }

  if (installed) {
    noteLevel();
    applyCvars();
#ifdef PROXY_GAME_EZ2
    static bool portalWindows = false;
    if (!portalWindows)
      portalWindows = installPortalWindows();
#endif
  }

  return installed;
}

} // namespace proxy::engine

// ---- Flashlight / Projected Lights ----

namespace proxy::engine_lights {
namespace {

struct FlashlightState {
  float origin[3];        // 0x00
  float orientation[4];   // 0x0C (quaternion x, y, z, w)
  float nearZ;            // 0x1C
  float farZ;             // 0x20
  float horizontalFov;    // 0x24
  float verticalFov;      // 0x28
  float quadratic;        // 0x2C
  float linear;           // 0x30
  float constant;         // 0x34
  float color[4];         // 0x38 (r, g, b, a)
  void *spotlightTexture; // 0x48
  int32_t spotlightFrame; // 0x4C
};
static_assert(sizeof(FlashlightState) == 0x50);

std::mutex g_projectedLock;
std::unordered_map<uint16_t, FlashlightState> g_projectedStates;

using UpdateFlashlightStateFn = void(PROXY_THISCALL *)(void *, uint16_t, const FlashlightState *);
UpdateFlashlightStateFn g_origUpdateFlashlightState = nullptr;

using DestroyShadowFn = void(PROXY_THISCALL *)(void *, uint16_t);
DestroyShadowFn g_origDestroyShadow = nullptr;

void PROXY_THISCALL hookUpdateFlashlightState(void *self, uint16_t handle, const FlashlightState *state) {
  if (state && handle != 0xffff) {
    std::lock_guard<std::mutex> lock(g_projectedLock);
    g_projectedStates[handle] = *state;
  }
  if (g_origUpdateFlashlightState)
    g_origUpdateFlashlightState(self, handle, state);
}

void PROXY_THISCALL hookDestroyShadow(void *self, uint16_t handle) {
  {
    std::lock_guard<std::mutex> lock(g_projectedLock);
    g_projectedStates.erase(handle);
  }
  if (g_origDestroyShadow)
    g_origDestroyShadow(self, handle);
}

std::mutex g_snapshotLock;
std::deque<std::pair<uint32_t, std::shared_ptr<const Snapshot>>> g_pending;
std::shared_ptr<const Snapshot> g_current;
std::shared_ptr<const Snapshot> g_published;
uint32_t g_sequence = 0;

bool finite3(const float *v) {
  return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

float envScale(const char *name, float fallback) {
  char v[32] = {};
  const float value = GetEnvironmentVariableA(name, v, sizeof(v)) ? float(std::atof(v)) : 0;
  return value > 0 ? value : fallback;
}

bool fromProjected(const FlashlightState &s, Light &l) {
  static const float brightness = 0.25f * envScale("OPENREMIX_PROXY_PROJECTED_SCALE", 3.0f);
  static const float fixedReference = envScale("OPENREMIX_PROXY_PROJECTED_REFERENCE", 0.0f);
  const float *q = s.orientation;
  const float norm = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
  if (!finite3(s.origin) || !(norm > 0.9f && norm < 1.1f) || !(s.farZ > 0) || !std::isfinite(s.farZ))
    return false;
  const float fov = std::max(s.horizontalFov, s.verticalFov);
  if (!(fov > 0 && fov < 180) || !finite3(s.color) || !finite3(&s.quadratic))
    return false;
  const float reference = fixedReference > 0 ? fixedReference : std::clamp(s.farZ * 0.25f, 64.0f, 256.0f);
  const float attenuation = std::clamp(
      s.constant + s.linear / reference + s.quadratic / (reference * reference), 0.0f, 1.0f);
  float sum = 0;
  for (int i = 0; i < 3; ++i)
    sum += l.color[i] = std::max(s.color[i], 0.0f) * brightness * attenuation;
  if (!(sum > 0))
    return false;

  l.type = 3; // Spot light
  std::memcpy(l.position, s.origin, 12);
  // Forward: +X of rotated basis
  l.direction[0] = 1 - 2 * (q[1] * q[1] + q[2] * q[2]);
  l.direction[1] = 2 * (q[0] * q[1] + q[3] * q[2]);
  l.direction[2] = 2 * (q[0] * q[2] - q[3] * q[1]);
  // Up: +Z of rotated basis
  l.up[0] = 2 * (q[0] * q[2] + q[3] * q[1]);
  l.up[1] = 2 * (q[1] * q[2] - q[3] * q[0]);
  l.up[2] = 1 - 2 * (q[0] * q[0] + q[1] * q[1]);
  l.fov = s.horizontalFov;
  l.cookie = engine_materials::textureNameOf(s.spotlightTexture);
  l.cookieFrame = std::max(s.spotlightFrame, 0);
  l.attenuation[0] = 0;
  l.attenuation[1] = 0;
  l.attenuation[2] = 1.0f / (reference * reference);
  const float outer = std::clamp(fov * 0.5f, 1.0f, 89.0f) * 0.01745329252f;
  l.phiDot = std::cos(outer);
  l.thetaDot = std::cos(outer * 0.5f);
  return true;
}

bool sameLights(const std::vector<Light> &a, const std::vector<Light> &b) {
  if (a.size() != b.size())
    return false;
  const auto floats = [](const float *x, const float *y, size_t n) { return !std::memcmp(x, y, n * sizeof(float)); };
  for (size_t i = 0; i < a.size(); ++i) {
    const Light &x = a[i], &y = b[i];
    if (x.hash != y.hash || x.type != y.type || x.cookie != y.cookie || x.cookieFrame != y.cookieFrame ||
        !floats(x.color, y.color, 3) || !floats(x.position, y.position, 3) || !floats(x.direction, y.direction, 3) ||
        !floats(x.attenuation, y.attenuation, 3) || x.thetaDot != y.thetaDot || x.phiDot != y.phiDot ||
        !floats(x.up, y.up, 3) || x.fov != y.fov)
      return false;
  }
  return true;
}

} // namespace

bool install() {
  static bool installed = false;
  if (installed)
    return true;

  void *shadowMgr = engine::createInterface("engine.dll", "VEngineShadowMgr002");
  if (!shadowMgr)
    return false;

  void **shadowVtable = *static_cast<void ***>(shadowMgr);
  // Slot 16: UpdateFlashlightState
  engine::swapSlot(&shadowVtable[16], reinterpret_cast<void *>(&hookUpdateFlashlightState),
                   reinterpret_cast<void **>(&g_origUpdateFlashlightState));
  // Slot 1: DestroyShadow
  engine::swapSlot(&shadowVtable[1], reinterpret_cast<void *>(&hookDestroyShadow),
                   reinterpret_cast<void **>(&g_origDestroyShadow));

  installed = true;
  proxy::diag::stage("EZ2: engine_lights installed (UpdateFlashlightState slot 16, DestroyShadow slot 1)");
  return true;
}

void sample() {
  auto snapshot = std::make_shared<Snapshot>();
  {
    std::lock_guard<std::mutex> lock(g_projectedLock);
    for (const auto &[handle, s] : g_projectedStates) {
      Light l;
      l.hash = 0x70726f6a74657800ull ^ (uint64_t(handle) * 0x9e3779b97f4a7c15ull);
      if (fromProjected(s, l))
        snapshot->projected.push_back(l);
    }
    std::sort(snapshot->projected.begin(), snapshot->projected.end(),
              [](const Light &a, const Light &b) { return a.hash < b.hash; });
  }

  if (g_published && sameLights(g_published->projected, snapshot->projected))
    return;

  g_published = snapshot;
  const uint32_t sequence = ++g_sequence;
  {
    std::lock_guard<std::mutex> lock(g_snapshotLock);
    g_current = snapshot;
    g_pending.emplace_back(sequence, snapshot);
    if (g_pending.size() > 64)
      g_pending.pop_front();
  }

  char text[48];
  std::snprintf(text, sizeof(text), "openremix:lights %u", sequence);
  engine::marker(text);
}

bool stylesActive() { return false; }

void activate(uint32_t sequence) {
  std::lock_guard<std::mutex> lock(g_snapshotLock);
  while (!g_pending.empty() && int32_t(sequence - g_pending.front().first) >= 0) {
    g_current = std::move(g_pending.front().second);
    g_pending.pop_front();
  }
}

std::shared_ptr<const Snapshot> current() {
  std::lock_guard<std::mutex> lock(g_snapshotLock);
  return g_current;
}

void report(uint32_t, uint32_t, uint32_t) {}
void dumpStatistics(FILE *) {}

} // namespace proxy::engine_lights
