// The x86 engine side (Left 4 Dead 2, Portal 2): install, markers, map, cvars,
// static prop skip (see x86_engine.h). The library loader (remixapi_lib_*) is
// remix_output's.
#define REMIX_WINAPI_NO_LIBRARY_LOADER
#include "x86_engine.h"

#include "bsp_world.h"
#include "diag.h"
#include "engine_hooks.h"
#ifdef PROXY_GAME_PORTAL2
#include "engine_lights.h"
#include "portal2.h"
#endif
#include "game.h"
#ifdef PROXY_GAME_L4D2
#include "l4d2.h"
#endif
#include "remix_output.h"

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>
#include <vector>

namespace proxy::x86 {

namespace {

// ---- what differs between the engines ----
//
// Portal 2's engine is L4D2's branch: the same classes and hooks, other
// offsets and interface slots, and its render methods take the render context
// first (IMatRenderContext *, as in later engines). Each function a hook
// replaces or the proxy calls is checked against the bytes it expects (? any
// byte) at offsets from its start.
struct Check {
  uintptr_t offset;
  const char *bytes;
};
template <size_t N> bool checks(const Module &module, uintptr_t rva, const Check (&list)[N]) {
  for (const Check &c : list)
    if (!bytesAt(module, rva + c.offset, c.bytes))
      return false;
  return true;
}

#ifdef PROXY_GAME_L4D2
constexpr char kGame[] = "L4D2";
// client.dll's current view id (CViewRender's g_CurrentViewID), from
// SetupCurrentView: "mov [g_CurrentViewID],edx; add esp,..".
Pattern g_viewIdPattern{"viewid", "89 15 ? ? ? ? 83 C4", 0x1CCFCC};
constexpr char kViewIdOpcode[] = "89 15";
// IMaterialSystem (VMaterialSystem080) slot 96: GetRenderContext (the calling
// thread's context, AddRef'd). The hardware context's SetPIXMarker (137)
// jumps to IShaderAPIDX8 slot 181 (game.h). Verified in L4D2's
// materialsystem.dll.
constexpr unsigned kSlotGetRenderContext = 96;
// IVEngineClient (VEngineClient013) slot 53: GetLevelName.
constexpr char kEngineClientInterface[] = "VEngineClient013";
// VEngineCvar007 (vstdlib.dll) slot 13: ConVar *FindVar(const char *).
constexpr unsigned kSlotFindVar = 13;
// IVRenderView::DrawWorldLists (VEngineRenderView013 slot 13) forwards to
// CRender::DrawWorldLists (vtable +0x20) and pops 12 bytes: push ebp; mov
// ebp,esp; mov ecx,[g_EngineRenderer] ... mov eax,[eax+0x20].
constexpr Check kDrawWorldLists[] = {{0, "55 8B EC 8B 0D"}, {0x11, "8B 40 20"}};
// COverlayMgr::RenderOverlays (its vtable slot 7, pops 4 bytes).
constexpr Check kRenderOverlays[] = {{0, "55 8B EC 81 EC 68 06 00 00"}};
// CModelRender::DrawModelExecute (VEngineModel016 slot 19): push ebp; mov
// ebp,esp; sub esp,0x28C.
constexpr Check kDrawModelExecute[] = {{0, "55 8B EC 81 EC 8C 02 00 00"}};
#else // PROXY_GAME_PORTAL2
constexpr char kGame[] = "Portal 2";
// g_CurrentViewID from the one place that writes VIEW_ILLEGAL (-2) to it (the
// monitor view's, the same function as L4D2's own -2 store);
// SetupCurrentView's own write is a register's.
Pattern g_viewIdPattern{"viewid", "C7 05 ? ? ? ? FE FF FF FF", 0x1F107F};
constexpr char kViewIdOpcode[] = "C7 05";
// IMaterialSystem (VMaterialSystem080, 165 slots) slot 113: GetRenderContext
// (the thread's context from TLS, AddRef'd, as L4D2's 96). IMatRenderContext
// slots 137 (SetPIXMarker, to IShaderAPIDX8 slot 186) and 1 (Release) are
// L4D2's.
constexpr unsigned kSlotGetRenderContext = 113;
// IVEngineClient (VEngineClient015) slot 53: GetLevelName, as L4D2's (52 is
// GetLevelNameShort; its "dedicated" is "Dedicated Server").
constexpr char kEngineClientInterface[] = "VEngineClient015";
// VEngineCvar007 slot 16 (IAppSystem has three slots more): FindVar, L4D2's
// code (FindCommandBase, slot 14, then not a command).
constexpr unsigned kSlotFindVar = 16;
// DrawWorldLists(IMatRenderContext *, IWorldRenderList *, unsigned long
// flags, float waterZAdjust): slot 13, the render context first, pops 16.
constexpr Check kDrawWorldLists[] = {{0, "55 8B EC 8B 0D ? ? ? ? D9 45 14"}, {0x11, "8B 40 20"}, {0x24, "C2 10 00"}};
// RenderOverlays(IMatRenderContext *, int nSortGroup): slot 7, pops 8.
constexpr Check kRenderOverlays[] = {{0, "55 8B EC 81 EC 44 06 00 00 53 57 8B F9"}};
// DrawModelExecute(IMatRenderContext *, const DrawModelState_t &, const
// ModelRenderInfo_t &, matrix3x4_t *): slot 19, pops 16; the info (third
// argument) is read first, its flags at +0x2C as in L4D2.
constexpr Check kDrawModelExecute[] = {{0, "55 8B EC 81 EC 7C 02 00 00 53 8B 5D 10 8B 43 2C"}};
#endif
// IMatRenderContext slot 137: SetPIXMarker (color, name), slot 1: Release.
constexpr unsigned kSlotContextSetPIXMarker = 137, kSlotContextRelease = 1;
// IVEngineClient::GetLevelName: "maps/x.bsp" while connected (signon state >=
// 2), "" otherwise, "dedicated" on a dedicated server: "cmp [dedicated],0; je
// ..; mov eax,"dedicated"" (both games).
constexpr unsigned kSlotGetLevelName = 53;
constexpr char kGetLevelName[] = "80 3D ? ? ? ? 00 74 06 B8";
// ICvar::FindVar: "mov eax,[ecx]; mov edx,[ebp+8]; mov eax,[eax+..]"
// (FindCommandBase) (both games).
constexpr char kFindVar[] = "55 8B EC 8B 01 8B 55 08 8B 40";

std::atomic<const char *> g_status{"not installed"};
Module g_engine, g_client, g_materialSystem;

// ---- markers ----

void *g_materials = nullptr;
std::atomic<uint64_t> g_markers{0};
// The queued render context keeps the marker's pointer, not its text, until
// the render thread runs it (frames later): a marker built on the stack
// ("openremix:portals 12") was read after it was gone and arrived as
// garbage, if at all. Each marker's text is copied here first, into a ring
// that outlives the queue (a frame sends a few dozen).
constexpr unsigned kMarkerRing = 4096, kMarkerLength = 96;
char g_markerTexts[kMarkerRing][kMarkerLength];
std::atomic<unsigned> g_markerNext{0};

// ---- view id ----

const int *g_viewId = nullptr;

// ---- the game window, the main thread ----

std::atomic<HWND> g_window{nullptr};
std::atomic<DWORD> g_mainThread{0};

// ---- IVRenderView::DrawWorldLists, COverlayMgr::RenderOverlays ----

struct Hook {
  const char *name;
  std::atomic<const char *> state{"not installed"};
  uintptr_t rva = 0; // the hooked function in its module
};
Hook g_worldListsHook{"DrawWorldLists"}, g_overlaysHook{"RenderOverlays"}, g_modelHook{"DrawModelExecute"};

#ifdef PROXY_GAME_PORTAL2
using DrawWorldListsFn = void(PROXY_THISCALL *)(void *, void *, void *, unsigned long, float);
using RenderOverlaysFn = void(PROXY_THISCALL *)(void *, void *, int);
using DrawModelExecuteFn = void(PROXY_THISCALL *)(void *, void *, const void *, const uint8_t *, void *);
#else
using DrawWorldListsFn = void(PROXY_THISCALL *)(void *, void *, unsigned long, float);
using RenderOverlaysFn = void(PROXY_THISCALL *)(void *, int);
using DrawModelExecuteFn = void(PROXY_THISCALL *)(void *, const void *, const uint8_t *, void *);
#endif
DrawWorldListsFn g_drawWorldLists = nullptr;
RenderOverlaysFn g_renderOverlays = nullptr;
DrawModelExecuteFn g_drawModelExecute = nullptr;

struct Statistics {
  std::atomic<uint64_t> worldLists{0}, worldListsSky{0}, overlays{0}, overlaysSky{0}, propsSkipped{0},
      propsDrawn{0}, mainFrames{0}, cvarPasses{0}, worldListsOther{0}, overlaysOther{0}, worldListsPortal{0},
      overlaysPortal{0};
} g_stats;

// ---- per frame ----

std::atomic<uint32_t> g_presents{0};
uint32_t g_lastMainFrame = ~0u; // main thread

// ---- the map ----

// IVEngineClient::GetLevelName (kSlotGetLevelName).
void *g_engineClient = nullptr;
std::mutex g_mapLock;
std::string g_announced, g_candidate; // under g_mapLock
unsigned g_stable = 0;
std::atomic<uint32_t> g_loads{0}, g_unloads{0};

// ---- cvars ----

// VEngineCvar007 (vstdlib.dll) FindVar (kSlotFindVar).
// ConVar (L4D2, Portal 2): ConCommandBase {vtable, next, registered, name
// +0x0C, help, flags +0x14}, IConVar at +0x18 (slot 1: SetValue(int),
// forwards to the parent, +0x1C). Verified in both engine.dlls' ConVar
// vtables (the IConVar part is the same code).
void *g_cvars = nullptr;
struct Forced {
  const char *name;
  int value;
  bool set; // false: only made usable (cheat, hidden and development flags cleared)
  void *var = nullptr;
  bool skipped = false; // OPENREMIX_PROXY_CVAR_SKIP
};
std::vector<Forced> g_forced; // main thread
std::atomic<const char *> g_cvarState{"not started"};
std::atomic<unsigned> g_cvarsFound{0}, g_cvarsMissing{0};
std::string g_cvarMissingNames; // main thread; read by the statistics (a snapshot below)
std::mutex g_cvarNamesLock;

} // namespace

// ---- markers, view id, threads ----

void marker(const char *text) {
  void *materials = g_materials;
  if (!materials)
    return;
  using GetRenderContextFn = void *(PROXY_THISCALL *)(void *);
  using SetPIXMarkerFn = void(PROXY_THISCALL *)(void *, unsigned long, const char *);
  using ReleaseFn = int(PROXY_THISCALL *)(void *);
  void *context =
      reinterpret_cast<GetRenderContextFn>((*static_cast<void ***>(materials))[kSlotGetRenderContext])(materials);
  if (!context)
    return;
  char *copy = g_markerTexts[g_markerNext.fetch_add(1, std::memory_order_relaxed) % kMarkerRing];
  std::snprintf(copy, kMarkerLength, "%s", text);
  void **vtable = *static_cast<void ***>(context);
  reinterpret_cast<SetPIXMarkerFn>(vtable[kSlotContextSetPIXMarker])(context, 0x0E5C0DE0, copy);
  reinterpret_cast<ReleaseFn>(vtable[kSlotContextRelease])(context);
  ++g_markers;
}

int viewId() { return g_viewId ? *g_viewId : -1; }

void setGameWindow(HWND window) {
  if (!window)
    return;
  g_window = window;
  g_mainThread = GetWindowThreadProcessId(window, nullptr);
}

bool onMainThread() {
  if (!g_mainThread.load()) {
    // Not handed over (OpenRemix off): the engine's window class.
    HWND window = FindWindowA("Valve001", nullptr);
    DWORD process = 0;
    if (window && GetWindowThreadProcessId(window, &process) && process == GetCurrentProcessId())
      setGameWindow(window);
  }
  return g_mainThread.load() == GetCurrentThreadId();
}

namespace {

// ---- cvars ----

// What l4d2-rtx forced in OpenRemix mode (main_module.cpp force_cvars and the
// map-load list; game_settings defaults lod_forcing off, force_graphic_settings
// on, enable_3d_sky off). Portal 2 has every forced one (the same reasons
// hold: its flashlights are env_projectedtexture's); some of those only made
// usable are L4D2's own.
void buildCvarTable() {
  const bool sky = envFlag("3DSKY", false); // 1: r_3dsky stays the game's (default on)
  g_forced = {
      // The proxy takes its signals as markers queued with the draws: the
      // material system renders on its own thread.
      {"mat_queue_mode", 2, true},
      // The proxy skins on the GPU from the game's bone matrices.
      {"mat_softwareskin", 0, true},
      // The proxy keeps D3D9 depth: the sun glow's occlusion query works.
      {"r_dopixelvisibility", 1, true},
      // The path tracer shadows everything; Source's blob and render-to-texture
      // shadows would draw dark quads.
      {"r_shadows", 0, true},
      {"r_shadowrendertotexture", 0, true},
      {"r_shadowfromworldlights", 0, true},
      // The flashlight is a Remix light (L4D2: l4d2_lights; Portal 2's
      // env_projectedtextures: engine_lights, which the client still updates
      // with these at 0); Source's projected one re-draws lit geometry.
      {"r_flashlightrender", 0, true},
      {"r_flashlightrendermodels", 0, true},
      {"r_flashlightrenderworld", 0, true},
      {"r_FlashlightDetailProps", 0, true},
      // Water views: culling near water surfaces, and their cost.
      {"r_WaterDrawRefraction", 0, true},
      {"r_WaterDrawReflection", 0, true},
      // Anti-culling (x86_cull): r_PortalTestEnts 0 makes the client's
      // renderable culling ask CullBox, which the patch skips.
      {"r_threaded_particles", 0, true},
      {"r_entityclips", 0, true},
      {"r_PortalTestEnts", 0, true},
      // Fast paths that bypass the draws the proxy captures (and
      // DrawModelExecute: the static prop skip).
      {"cl_fastdetailsprites", 0, true},
      {"cl_brushfastpath", 0, true},
      {"cl_tlucfastpath", 0, true},
      // Not cl_modelfastpath: common infected's wounds reach the shader only
      // through its instance data (no wound there: the shader API binds
      // ellipsoids at 1e6), and static props still pass DrawModelExecute.
      {"r_queued_ropes", 0, true},
      {"mat_softwarelighting", 0, true},
      {"mat_parallaxmap", 0, true},
      {"mat_frame_sync_enable", 0, true},
      {"mat_dof_enabled", 0, true},
      {"mat_displacementmap", 0, true},
      {"mat_drawflat", 0, true},
      {"mat_normalmaps", 0, true},
      {"mat_fullbright", 1, true},
      {"mat_phong", 1, true},
      {"mat_fastnobump", 1, true},
      {"mat_disable_bloom", 1, true},
      {"r_threadeddetailprops", 0, true},
      {"r_DrawDetailProps", 0, true},
      {"r_DispWalkable", 1, true},
      // force_graphic_settings.
      {"cpu_level", 2, true},
      {"gpu_level", 0, true},
      {"gpu_mem_level", 2, true},
      // Effects.
      {"cl_particle_fallback_base", 0, true},
      {"cl_particle_fallback_multiplier", 1, true},
      {"cl_impacteffects_limit_general", 10, true},
      {"cl_impacteffects_limit_exit", 3, true},
      {"cl_impacteffects_limit_water", 2, true},
      {"mat_depthfeather_enable", 0, true},
      {"mat_force_vertexfog", 1, true},
      {"cl_footstep_fx", 1, true},
      {"cl_ragdoll_self_collision", 1, true},
      {"cl_ragdoll_maxcount", 24, true},
      {"cl_detaildist", 1024, true},
      {"cl_detailfade", 400, true},
      {"r_drawmodeldecals", 1, true},
      {"r_decalstaticprops", 1, true},
      {"cl_player_max_decal_count", 32, true},
      {"r_decals", 2048, true},
      {"r_decal_overlap_count", 3, true},
      // Fog is the runtime's.
      {"fog_override", 1, true},
      {"fog_enable", 0, true},
      // Usable from the console (l4d2-rtx: on map load; lod_forcing off).
      {"r_staticprop_lod", 0, false},
      {"r_lod", 0, false},
      {"r_lod_switch_scale", 0, false},
  };
  g_forced.push_back({"r_3dsky", 0, !sky});
  auto usable = [](std::initializer_list<const char *> names) {
    for (const char *name : names) {
      bool listed = false;
      for (const Forced &f : g_forced)
        listed |= _stricmp(f.name, name) == 0;
      if (!listed)
        g_forced.push_back({name, 0, false});
    }
  };
  usable({"r_propsmaxdist", "cl_detaildist", "cl_detailfade", "cl_footstep_fx", "cl_fov", "cl_interpolate",
          "fx_drawimpactdebris", "fx_drawimpactdust", "fx_drawmetalspark", "r_drawbatchdecals", "r_drawflecks",
          "r_fade360style", "r_frustumcullworld", "r_maxmodeldecal", "r_occlusion", "r_particle_timescale",
          "r_queued_decals", "r_RainParticleDensity", "r_ropetranslucent", "r_ShowViewerArea", "r_snapportal",
          "r_staticlight_streams", "r_staticpropinfo", "r_teeth"});
#ifdef PROXY_GAME_L4D2
  usable({"cl_impacteffects", "cl_particle_batch_mode", "cl_smoke_alpha", "cl_smoke_far", "cl_viewbob",
          "r_draw_flashlight_3rd_person", "r_draw_lasersight_1st_person", "r_draw_lasersight_3rd_person",
          "r_drawunderwaterfogblocker", "r_flashlight_3rd_person_range", "r_impactparticles", "scene_print"});
#else
  // The portal views (see drawWorldLists): one level. At 0 the engine renders
  // none and draws every portal closed (its rims full of static, which the
  // replayed rims showed over the ray portals); the levels below the first
  // are the path tracer's (its teleports), and each one costs a view.
  if (portal2::enabled())
    g_forced.push_back({"r_portal_stencil_depth", 1, true});
  // The stencil path, for trying from the console; not forced.
  usable({"r_portal_stencil_depth", "r_portalstencildisable", "r_portal_fastpath", "r_drawportalfrustum"});
#endif
  // OPENREMIX_PROXY_CVAR_SKIP="name,name": made usable, not forced.
  char skip[1024] = {};
  if (GetEnvironmentVariableA("OPENREMIX_PROXY_CVAR_SKIP", skip, sizeof(skip))) {
    const std::string list = std::string(",") + skip + ",";
    for (Forced &f : g_forced)
      f.skipped = list.find(std::string(",") + f.name + ",") != std::string::npos;
  }
}

void forceCvars() {
  static const bool enabled = envFlag("CVARS", true);
  if (!enabled) {
    g_cvarState = "off (OPENREMIX_PROXY_CVARS=0)";
    return;
  }
  if (!g_cvars)
    return;
  static bool broken = false;
  if (broken)
    return;
  if (g_forced.empty())
    buildCvarTable();
  using FindVarFn = void *(PROXY_THISCALL *)(void *, const char *);
  using SetIntFn = void(PROXY_THISCALL *)(void *, int);
  const auto findVar = reinterpret_cast<FindVarFn>((*static_cast<void ***>(g_cvars))[kSlotFindVar]);
  // Names not found yet (client cvars before client.dll registers them) are
  // looked up again now and then.
  static unsigned pass = 0;
  const bool lookup = pass++ % 300 == 0;
  unsigned found = 0, missing = 0;
  std::string missingNames;
  for (Forced &f : g_forced) {
    if (!f.var && lookup) {
      void *var = findVar(g_cvars, f.name);
      if (var) {
        // The layout this relies on: the name at +0x0C.
        const char *name = *reinterpret_cast<const char *const *>(static_cast<uint8_t *>(var) + 0x0C);
        if (IsBadReadPtr(name, 1) || _stricmp(name, f.name) != 0) {
          broken = true;
          g_cvarState = "ConVar layout not as expected: nothing forced";
          diag::stage("%s: ConVar %s does not hold its name at +0x0C: cvar forcing off", kGame, f.name);
          return;
        }
        f.var = var;
      }
    }
    if (!f.var) {
      ++missing;
      missingNames += missingNames.empty() ? f.name : std::string(" ") + f.name;
      continue;
    }
    ++found;
    auto *bytes = static_cast<uint8_t *>(f.var);
    // FCVAR_DEVELOPMENTONLY, FCVAR_HIDDEN, FCVAR_CHEAT.
    *reinterpret_cast<int *>(bytes + 0x14) &= ~((1 << 1) | (1 << 4) | (1 << 14));
    if (f.set && !f.skipped) {
      void *conVar = bytes + 0x18; // IConVar (SetValue returns at once when unchanged)
      reinterpret_cast<SetIntFn>((*static_cast<void ***>(conVar))[1])(conVar, f.value);
    }
  }
  if (lookup) {
    std::lock_guard<std::mutex> lock(g_cvarNamesLock);
    g_cvarMissingNames = missingNames;
  }
  g_cvarsFound = found;
  g_cvarsMissing = missing;
  g_cvarState = "forced every frame";
  ++g_stats.cvarPasses;
}

// ---- once per frame on the main thread ----

void mainFrame() {
  const uint32_t present = g_presents.load(std::memory_order_relaxed);
  if (present == g_lastMainFrame)
    return;
  g_lastMainFrame = present;
  ++g_stats.mainFrames;
  forceCvars();
#ifdef PROXY_GAME_L4D2
  l4d2::sampleLights();
#else
  portal2::sample(); // the portals (announced with a marker when they changed)
#endif
}

// ---- the map ----

// Polled at every Present (the thread that runs the shader API's draws, where
// markers arrive): "openremix:map <name>" when the engine's level changes and
// stayed the same for two frames (the name is the main thread's, read while it
// may be written), "openremix:map" when it disconnects.
void pollMap() {
  if (!g_engineClient)
    return;
  using GetLevelNameFn = const char *(PROXY_THISCALL *)(void *);
  const char *level =
      reinterpret_cast<GetLevelNameFn>((*static_cast<void ***>(g_engineClient))[kSlotGetLevelName])(g_engineClient);
  char name[260] = {};
  if (level)
    std::snprintf(name, sizeof(name), "%s", level);
  if (std::strcmp(name, "dedicated") == 0)
    name[0] = 0;
  std::string text;
  {
    std::lock_guard<std::mutex> lock(g_mapLock);
    if (g_candidate != name) {
      g_candidate = name;
      g_stable = 1;
      return;
    }
    if (++g_stable < 2 || g_candidate == g_announced)
      return;
    g_announced = g_candidate;
    text = g_announced.empty() ? "openremix:map" : "openremix:map " + g_announced;
  }
  ++(text.size() > 13 ? g_loads : g_unloads);
  diag::stage("%s: %s", kGame, text.c_str());
  remix::onMarker(text.c_str());
}

// ---- hooks ----

// Which views get the world-lists and overlay markers (their world and
// overlays are the BSP's, and the main camera is the last such view's).
// L4D2: every view but the 3D skybox (its world is the BSP's only when it is
// path traced, in world space; otherwise the engine's sky view keeps its
// draws, as l4d2-rtx did; its overlays always stay).
// Portal 2: also the views Portal's renderer draws through the portals
// (stencil views drawn into the main target, inside the main view's
// translucent pass: after its world, with view ids of their own above the
// engine's, CPortalRender's from VIEW_ID_COUNT up). Marked as world views,
// the last of them would be taken for the main camera (remix_output's world
// views). They get markers of their own instead, "portal_view 1|0"
// (remix_output: their world draws dropped, the BSP's world stands in; the
// view's later draws, the models it shows, kept as instances deduplicated
// against the main view's; never a camera), and the overlay markers (the
// BSP's overlays stand in). With portals off (portal2.h) or their views not
// kept, they are left to the engine as before, as are their 3D skyboxes,
// water and shadow views. An unknown view id (-1: the pattern was not found)
// is marked, as in L4D2.
enum class WorldMark { None, World, Portal };
bool portalView(int view) {
#ifdef PROXY_GAME_PORTAL2
  return view >= 0 && view != kViewMain && view != kViewMonitor && view != kView3dSky && portal2::enabled() &&
         portal2::viewsKept();
#else
  (void)view;
  return false;
#endif
}
WorldMark worldListsView(int view) {
#ifdef PROXY_GAME_PORTAL2
  if (view != kViewMain && view != kViewMonitor && view != kView3dSky && view >= 0)
    return portalView(view) ? WorldMark::Portal : WorldMark::None;
#endif
  return view != kView3dSky || bsp::skyTracedReach() > 0 ? WorldMark::World : WorldMark::None;
}
bool overlaysView(int view) {
#ifdef PROXY_GAME_PORTAL2
  if (view != kViewMain && view != kViewMonitor && view >= 0 && !portalView(view))
    return false;
#endif
  return view != kView3dSky;
}

// Around IVRenderView::DrawWorldLists: what was marked.
WorldMark beginWorldLists() {
  const int view = viewId();
  if (view == kViewMain || view < 0)
    mainFrame();
#ifdef PROXY_GAME_PORTAL2
  // The projected textures, in every view as in GMod (idempotent: a marker
  // only when they changed).
  engine_lights::sample();
#endif
  const WorldMark mark = worldListsView(view);
  ++(mark == WorldMark::World    ? g_stats.worldLists
     : mark == WorldMark::Portal ? g_stats.worldListsPortal
     : view == kView3dSky        ? g_stats.worldListsSky
                                 : g_stats.worldListsOther);
  if (mark == WorldMark::World)
    marker("openremix:world_lists 1");
  else if (mark == WorldMark::Portal)
    marker("openremix:portal_view 1");
  return mark;
}
void endWorldLists(WorldMark mark) {
  if (mark == WorldMark::World)
    marker("openremix:world_lists 0");
  else if (mark == WorldMark::Portal)
    marker("openremix:portal_view 0");
}

// Overlays come from the BSP too, except in the 3D skybox (bsp_world leaves
// those to the engine) and the views above.
bool beginOverlays() {
  const int view = viewId();
  const bool mark = overlaysView(view);
  ++(mark ? (portalView(view) ? g_stats.overlaysPortal : g_stats.overlays)
          : view == kView3dSky ? g_stats.overlaysSky : g_stats.overlaysOther);
  if (mark)
    marker("openremix:overlays 1");
  return mark;
}
void endOverlays(bool mark) {
  if (mark)
    marker("openremix:overlays 0");
}

// IVModelRender::DrawModelExecute(.., const ModelRenderInfo_t &, ..): a static
// prop the BSP world draws itself (bsp::providesProp, by placement:
// ModelRenderInfo_t::pModelToWorld at +0x20) is not drawn (the proxy would
// drop its draws). The 3D skybox stays with the engine.
// OPENREMIX_PROXY_ENGINE_PROPS=1 draws them all.
bool skipModel(const uint8_t *info) {
  static const bool engineProps = envFlag("ENGINE_PROPS", false);
  if (!engineProps && info && viewId() != kView3dSky) {
    const float *modelToWorld = *reinterpret_cast<const float *const *>(info + 0x20);
    if (modelToWorld && bsp::providesProp(modelToWorld)) {
      ++g_stats.propsSkipped;
      return true;
    }
  }
  ++g_stats.propsDrawn;
  return false;
}

#ifdef PROXY_GAME_PORTAL2
// Portal 2's: the render context first (see kDrawWorldLists).
[[gnu::force_align_arg_pointer]] void PROXY_THISCALL drawWorldLists(void *self, void *context, void *list,
                                                                      unsigned long flags, float waterZ) {
  const WorldMark mark = beginWorldLists();
  g_drawWorldLists(self, context, list, flags, waterZ);
  endWorldLists(mark);
}
[[gnu::force_align_arg_pointer]] void PROXY_THISCALL renderOverlays(void *self, void *context, int sortGroup) {
  const bool mark = beginOverlays();
  g_renderOverlays(self, context, sortGroup);
  endOverlays(mark);
}
[[gnu::force_align_arg_pointer]] void PROXY_THISCALL drawModelExecute(void *self, void *context, const void *state,
                                                                        const uint8_t *info, void *bones) {
  if (!skipModel(info))
    g_drawModelExecute(self, context, state, info, bones);
}
#else
[[gnu::force_align_arg_pointer]] void PROXY_THISCALL drawWorldLists(void *self, void *list, unsigned long flags,
                                                                      float waterZ) {
  const WorldMark mark = beginWorldLists();
  g_drawWorldLists(self, list, flags, waterZ);
  endWorldLists(mark);
}
[[gnu::force_align_arg_pointer]] void PROXY_THISCALL renderOverlays(void *self, int sortGroup) {
  const bool mark = beginOverlays();
  g_renderOverlays(self, sortGroup);
  endOverlays(mark);
}
[[gnu::force_align_arg_pointer]] void PROXY_THISCALL drawModelExecute(void *self, const void *state,
                                                                        const uint8_t *info, void *bones) {
  if (!skipModel(info))
    g_drawModelExecute(self, state, info, bones);
}
#endif

// ---- install ----

bool slotInCode(void *object, unsigned slot, const Module &module) {
  return object && module.inCode((*static_cast<void ***>(object))[slot]);
}

void installWorldLists(void *renderView) {
  // Slot 13: IVRenderView::DrawWorldLists (kDrawWorldLists).
  if (!slotInCode(renderView, 13, g_engine)) {
    g_worldListsHook.state = "slot 13 not in engine.dll";
    return;
  }
  void **slot = &(*static_cast<void ***>(renderView))[13];
  const auto *fn = static_cast<const uint8_t *>(*slot);
  g_worldListsHook.rva = uintptr_t(fn) - g_engine.base;
  if (!checks(g_engine, g_worldListsHook.rva, kDrawWorldLists)) {
    g_worldListsHook.state = "not DrawWorldLists (bytes differ)";
    return;
  }
  g_drawWorldLists = reinterpret_cast<DrawWorldListsFn>(*slot);
  g_worldListsHook.state =
      swapSlot(slot, reinterpret_cast<void *>(&drawWorldLists)) ? "ok" : "cannot write the vtable";
}

void installOverlays() {
  // COverlayMgr's vtable by its RTTI (engine.dll+0x37661C in the L4D2 build
  // this was written for, +0x3729A8 in Portal 2's), slot 7: RenderOverlays
  // (kRenderOverlays).
  void **vtable = rttiVtable(g_engine, ".?AVCOverlayMgr@@");
  if (!vtable) {
    g_overlaysHook.state = "COverlayMgr vtable not found";
    return;
  }
  if (!g_engine.inCode(vtable[7])) {
    g_overlaysHook.state = "slot 7 not in engine.dll";
    return;
  }
  g_overlaysHook.rva = uintptr_t(vtable[7]) - g_engine.base;
  if (!checks(g_engine, g_overlaysHook.rva, kRenderOverlays)) {
    g_overlaysHook.state = "not RenderOverlays (bytes differ)";
    return;
  }
  diag::stage("%s: COverlayMgr vtable at engine+0x%lX", kGame, (unsigned long)(uintptr_t(vtable) - g_engine.base));
  g_renderOverlays = reinterpret_cast<RenderOverlaysFn>(vtable[7]);
  g_overlaysHook.state =
      swapSlot(&vtable[7], reinterpret_cast<void *>(&renderOverlays)) ? "ok" : "cannot write the vtable";
}

void installModelRender() {
  void *modelRender = createInterface("engine.dll", "VEngineModel016");
  if (!slotInCode(modelRender, 19, g_engine)) {
    g_modelHook.state = "VEngineModel016 slot 19 not found";
    return;
  }
  void **slot = &(*static_cast<void ***>(modelRender))[19];
  g_modelHook.rva = uintptr_t(*slot) - g_engine.base;
  // CModelRender::DrawModelExecute (kDrawModelExecute).
  if (!checks(g_engine, g_modelHook.rva, kDrawModelExecute)) {
    g_modelHook.state = "not DrawModelExecute (bytes differ)";
    return;
  }
  g_drawModelExecute = reinterpret_cast<DrawModelExecuteFn>(*slot);
  g_modelHook.state =
      swapSlot(slot, reinterpret_cast<void *>(&drawModelExecute)) ? "ok" : "cannot write the vtable";
}

// A slot whose function starts with `bytes`.
bool slotIs(void *object, unsigned slot, const Module &module, const char *bytes) {
  return slotInCode(object, slot, module) &&
         bytesAt(module, uintptr_t((*static_cast<void ***>(object))[slot]) - module.base, bytes);
}

bool installEngine() {
  if (!g_engine.open("engine.dll") || !g_materialSystem.open("materialsystem.dll"))
    return false;
  void *renderView = createInterface("engine.dll", "VEngineRenderView013");
  void *materials = createInterface("materialsystem.dll", "VMaterialSystem080");
  g_engineClient = createInterface("engine.dll", kEngineClientInterface);
  g_cvars = createInterface("vstdlib.dll", "VEngineCvar007");
  if (!renderView || !materials)
    return false;
  if (!slotInCode(materials, kSlotGetRenderContext, g_materialSystem)) {
    static char status[96];
    std::snprintf(status, sizeof(status),
                  "IMaterialSystem slot %u not in materialsystem.dll: no markers, nothing installed",
                  kSlotGetRenderContext);
    g_status = status;
    diag::stage("%s: %s", kGame, g_status.load());
    return true;
  }
  if (g_engineClient && !slotIs(g_engineClient, kSlotGetLevelName, g_engine, kGetLevelName))
    g_engineClient = nullptr;
  {
    Module vstdlib;
    if (g_cvars && !(vstdlib.open("vstdlib.dll") && slotIs(g_cvars, kSlotFindVar, vstdlib, kFindVar)))
      g_cvars = nullptr;
  }
  g_materials = materials;
  installWorldLists(renderView);
  installOverlays();
  if (envFlag("ENGINE_PROPS", false))
    g_modelHook.state = "off (OPENREMIX_PROXY_ENGINE_PROPS=1)";
  else
    installModelRender();
#ifdef PROXY_GAME_PORTAL2
  // The shadow manager's flashlight hooks (engine.dll only), before any map:
  // the projected textures are announced with the markers above.
  engine_lights::install();
#endif
  g_status = "engine.dll hooked; waiting for client.dll";
  diag::stage("%s: engine hooks: DrawWorldLists %s, RenderOverlays %s, DrawModelExecute %s; GetLevelName %s, "
              "cvars %s",
              kGame, g_worldListsHook.state.load(), g_overlaysHook.state.load(), g_modelHook.state.load(),
              g_engineClient ? "ok" : "MISSING", g_cvars ? "ok" : "MISSING");
  return true;
}

bool installClient() {
  if (!g_client.open("client.dll"))
    return false;
  if (g_viewIdPattern.find(g_client) && bytesAt(g_client, g_viewIdPattern.rva, kViewIdOpcode)) {
    const auto address = *g_client.at<const uintptr_t>(g_viewIdPattern.rva + 2);
    if (g_client.holds(reinterpret_cast<const void *>(address)))
      g_viewId = reinterpret_cast<const int *>(address);
  }
  diag::stage("%s: view id %s (client+0x%lX, %u matches)", kGame,
              g_viewId ? "found" : "NOT found: every view marked", (unsigned long)g_viewIdPattern.rva,
              g_viewIdPattern.count);
  if (!g_materials)
    return true; // nothing on the engine side either (no markers)
  installCull(g_engine, g_client);
#ifdef PROXY_GAME_L4D2
  l4d2::installLights();
#else
  portal2::install();
#endif
  g_status = "installed";
  return true;
}

} // namespace

void dumpEngineStatistics(FILE *out) {
  static ULONGLONG last = GetTickCount64();
  const ULONGLONG now = GetTickCount64();
  const double seconds = now > last ? double(now - last) / 1000 : 1;
  last = now;
  auto rate = [&](std::atomic<uint64_t> &counter) { return double(counter.exchange(0)) / seconds; };
  fprintf(out, "== %s engine: %s | view id %s (client+0x%lX, %u matches)\n", kGame, g_status.load(),
          g_viewId ? "ok" : "missing", (unsigned long)g_viewIdPattern.rva, g_viewIdPattern.count);
  for (const Hook *hook : {&g_worldListsHook, &g_overlaysHook, &g_modelHook})
    fprintf(out, "   %s: %s (engine+0x%lX)\n", hook->name, hook->state.load(), (unsigned long)hook->rva);
  fprintf(out,
          "   per second: world lists marked %.1f, 3D sky left to the engine %.1f | overlays marked %.1f, 3D sky "
          "%.1f | static props skipped %.1f, models drawn %.1f | main frames %.1f, markers %.1f\n",
          rate(g_stats.worldLists), rate(g_stats.worldListsSky), rate(g_stats.overlays), rate(g_stats.overlaysSky),
          rate(g_stats.propsSkipped), rate(g_stats.propsDrawn), rate(g_stats.mainFrames), rate(g_markers));
#ifdef PROXY_GAME_PORTAL2
  // The views left to the engine (worldListsView): portal views, their 3D
  // skyboxes, water and shadow views.
  fprintf(out,
          "   per second: portal views marked (portal_view) %.1f, their overlays %.1f | world lists of other views "
          "left to the engine %.1f, their overlays %.1f\n",
          rate(g_stats.worldListsPortal), rate(g_stats.overlaysPortal), rate(g_stats.worldListsOther),
          rate(g_stats.overlaysOther));
#endif
  {
    std::lock_guard<std::mutex> lock(g_mapLock);
    fprintf(out, "   map: \"%s\" (GetLevelName %s; %u loads, %u unloads)\n", g_announced.c_str(),
            g_engineClient ? "ok" : "missing", g_loads.load(), g_unloads.load());
  }
  {
    std::lock_guard<std::mutex> lock(g_cvarNamesLock);
    fprintf(out, "   cvars: %s, %u found, %u missing%s%s (%.1f passes/s)\n", g_cvarState.load(), g_cvarsFound.load(),
            g_cvarsMissing.load(), g_cvarMissingNames.empty() ? "" : ": ", g_cvarMissingNames.c_str(),
            rate(g_stats.cvarPasses));
  }
  dumpCull(out);
#ifdef PROXY_GAME_L4D2
  l4d2::dumpLights(out);
#else
  portal2::dumpStatistics(out);
#endif
}

} // namespace proxy::x86

// d3d_hooks' Present: installs what is not yet in place (retried until
// engine.dll, materialsystem.dll and client.dll are loaded), then the
// per-frame work of this thread. OPENREMIX_PROXY_ENGINE_HOOKS=0 leaves the
// engine untouched.
namespace proxy::engine {
bool install() {
  using namespace proxy::x86;
  g_presents.fetch_add(1, std::memory_order_relaxed);
  // The engine side serves OpenRemix only (OPENREMIX_PROXY=0: the game as it is).
  static const bool enabled = envFlag("ENGINE_HOOKS", true) && remix::enabled();
  if (!enabled) {
    g_status = remix::enabled() ? "off (OPENREMIX_PROXY_ENGINE_HOOKS=0)" : "off (OPENREMIX_PROXY=0)";
    return false;
  }
  static bool engineDone = false, clientDone = false;
  if (!engineDone)
    engineDone = installEngine();
  if (engineDone && !clientDone)
    clientDone = installClient();
  if (engineDone)
    pollMap();
  // Menus and loading screens draw no world: the main thread's per-frame work
  // runs here when the main thread presents (mat_queue_mode 0).
  if (clientDone && onMainThread())
    mainFrame();
  return engineDone && clientDone;
}
} // namespace proxy::engine
