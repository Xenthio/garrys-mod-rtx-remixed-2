// The x86 engine side (Left 4 Dead 2, Portal 2): anti-culling (l4d2-rtx
// main_module.cpp, its default anti-cull mode without map_settings). The path
// tracer needs what the camera does not see: the engine is made to draw the
// world near the camera whatever the frustum says, and entities and brush
// models without their culling.
//
//  * R_RecursiveWorldNode (engine): nodes are no longer tested against the
//    frustum ("contents < -1" -> "<= -1": only leaves reach R_CullNode), and a
//    leaf within OPENREMIX_PROXY_NOCULL_DISTANCE (default 600 units) of the
//    view is never culled (R_CullNode decides beyond it). The surfaces' back
//    face tests in it are taken out, and R_DrawLeaf's.
//  * Before the walk (R_BuildWorldLists), every leaf within that distance is
//    put in the current visframe with its parents (the walk only enters
//    nodes of the current visframe: the PVS).
//  * CBrushBatchRender::DrawOpaqueBrushModel: the back face test (as for
//    shadow depth). CDispInfo::Render: its CullBox test (L4D2; Portal 2's
//    has none).
//  * CClientLeafSystem::ExtractCulledRenderables (client): entities in the
//    listed leaves are kept without engine->CullBox (r_PortalTestEnts 0,
//    x86_engine's cvars). CSimpleWorldView::Setup: what is below a water
//    surface is drawn whether or not the view plane crosses it.
//
// The 3D skybox and monitor views keep the engine's culling (the distance
// rule and the leaf forcing look at the view id), in Portal 2 every view but
// the main one (the portal views, whose draws the proxy leaves to their own
// views: x86_engine.cpp). Every site is found by its pattern and checked byte
// for byte; a site that does not match is left out. The two engines' sites
// are side by side below. OPENREMIX_PROXY_ENGINE_NOCULL=0 leaves the engine's
// culling alone.
#include "x86_engine.h"

#include "diag.h"
#include "game.h"

#include <atomic>
#include <cstring>
#include <initializer_list>

namespace proxy::x86 {
namespace {

std::atomic<const char *> g_status{"not installed"};
float g_distance = 600;

using CullNodeFn = bool(__cdecl *)(const void *node);
CullNodeFn g_cullNode = nullptr;
const int *g_visframecount = nullptr;
const float *g_viewOrigin = nullptr;
uint8_t *const *g_worldBrush = nullptr; // host_state.worldbrush (worldbrushdata_t *)

struct Site {
  const char *name;
  Pattern pattern; // bytes null: the engine has no such site (or it is found from another one)
  std::atomic<const char *> state{"not installed"};
  size_t offset = 0; // the patched instruction, from the match
};
#ifdef PROXY_GAME_L4D2
constexpr char kGame[] = "L4D2";
// engine.dll
Site g_cullNode01{"R_RecursiveWorldNode node test", {"cullnode01", "7C ? 53 E8 ? ? ? ? 83 C4 04", 0xCD7E5}};
Site g_cullNodeFn{"R_CullNode", {"R_CullNode", "55 8B EC 80 3D ? ? ? ? ? 8B 4D", 0xFC490}};
Site g_cullReturn{"R_RecursiveWorldNode culled exit", {"cullnode_cull", "5F 8B C6 5E 5B 8B E5 5D C3 53", 0xCD935}};
Site g_cullSkip{"R_RecursiveWorldNode visible path",
                {"cullnode_skip", "? ? ? 0F 8D ? ? ? ? 8B 43 ? 8A 48 ? 80 F9 ? 77 ? 0F B6 D1", 0xCD7F8}};
Site g_backface01{"surface back face test 1", {"cullnode_backface01", "74 ? ? ? F7 C1", 0xCD8C1}};
Site g_backface02{"surface back face test 2", {"cullnode_backface02", "75 ? 8B D1 C1 EA", 0xCD8CB}};
Site g_drawLeaf{"R_DrawLeaf back face test", {"drawleaf_backface", "0F 87 ? ? ? ? 8B DA", 0xCD4E7}};
Site g_brushModel{"DrawOpaqueBrushModel back face test", {"draw_opaque_bmodel_backface", "74 ? 0F BF 4B", 0xD2250}};
Site g_dispInfo{"CDispInfo::Render CullBox", {"cdispinfo_render", "75 ? 53 38 45", 0xB13E5}};
Site g_preWorldNode{"leaf forcing (before R_RecursiveWorldNode)", {"pre_recursive_world_node", "8B 50 ? 8B CB E8", 0xD1648}};
Site g_visframe{"r_visframecount", {"visframecount", "3B 15 ? ? ? ? 75 ? 83 F8", 0xCD65A}};
Site g_origin{"current view origin",
              {"current_view_origin", "F3 0F 10 05 ? ? ? ? F3 0F 10 0D ? ? ? ? F3 0F 10 15 ? ? ? ? 53", 0xB9843}};
// client.dll
Site g_entities{"ExtractCulledRenderables CullBox", {"extract_culled_renderables", "74 ? 8B 45 ? FF 48 ? EB ? 0F 57 C0", 0xBDA76}};
Site g_water{"DoesViewPlaneIntersectWater", {"intersect_water", "74 ? 83 8E ? ? ? ? ? EB ? 80 7F", 0x1CF46F}};
// R_BuildWorldLists: "mov eax,[host_state.worldbrush]; mov edx,[eax+0x50]
// (nodes); mov ecx,ebx; call R_RecursiveWorldNode" (the stub below replays the
// middle two).
constexpr char kWorldBrushLoad[] = "A1";
constexpr uintptr_t kWorldBrushLoadAt = 5; // before the match: the opcode and the operand
constexpr char kPreWorldNode[] = "8B 50 50 8B CB E8";
// R_RecursiveWorldNode's "mov eax,[r_visframecount]" at +6, its operand at +7.
constexpr char kWalkVisframe[] = "A1";
constexpr uintptr_t kWalkVisframeAt = 6, kWalkVisframeOperand = 7;
// The view origin's x, y, z operands in g_origin's match.
constexpr uintptr_t kOriginOperands[3] = {4, 12, 20};
// worldbrushdata_t {.., numleafs +0x0C, leafs +0x10, .., numnodes +0x4C,
// nodes +0x50}.
constexpr size_t kNumLeafs = 0x0C, kLeafs = 0x10, kNumNodes = 0x4C, kNodes = 0x50;
#else // PROXY_GAME_PORTAL2
constexpr char kGame[] = "Portal 2";
// engine.dll: R_RecursiveWorldNode returns nothing here (L4D2's returns a
// count in eax): its culled exit is "pop edi; pop esi; pop ebx; mov esp,ebp;
// pop ebp; ret", found from the node test's jump (as a pattern it is
// everywhere).
Site g_cullNode01{"R_RecursiveWorldNode node test",
                  {"cullnode01", "7C ? 53 E8 ? ? ? ? 83 C4 04 84 C0 0F 85 ? ? ? ? 83 3B 00 0F 8D", 0xE7258}};
Site g_cullNodeFn{"R_CullNode", {"R_CullNode", "55 8B EC 80 3D ? ? ? ? ? 8B 4D", 0x10F950}};
Site g_cullReturn{"R_RecursiveWorldNode culled exit", {"cullnode_cull", nullptr, 0xE73A2}};
Site g_cullSkip{"R_RecursiveWorldNode visible path",
                {"cullnode_skip", "? ? ? 0F 8D ? ? ? ? 8B 43 ? 8A 48 ? 80 F9 ? 77 ? 0F B6 D1", 0xE726B}};
Site g_backface01{"surface back face test 1", {"cullnode_backface01", "74 ? 8B 0F F7 C1 00 00 02 00 75", 0xE7323}};
Site g_backface02{"surface back face test 2", {"cullnode_backface02", "75 ? 8B D1 C1 EA", 0xE732D}};
Site g_drawLeaf{"R_DrawLeaf back face test", {"drawleaf_backface", "0F 87 ? ? ? ? 8B D1 C1 EA 16 83 E2 03", 0xE6F23}};
Site g_brushModel{"DrawOpaqueBrushModel back face test",
                  {"draw_opaque_bmodel_backface", "74 ? 0F BF 4B 10 85 C9 0F 8E", 0x7196E}};
// CDispInfo::Render tests no frustum here (the displacements come with their
// leaves).
Site g_dispInfo{"CDispInfo::Render CullBox", {"cdispinfo_render", nullptr, 0}};
Site g_preWorldNode{"leaf forcing (before R_RecursiveWorldNode)", {"pre_recursive_world_node", "8B 52 54 8B CB E8", 0xE76CD}};
Site g_visframe{"r_visframecount", {"visframecount", "3B 15 ? ? ? ? 75 ? 83 F8", 0xE70DA}};
// The view origin as R_BuildWorldLists stores it for the walk (modelorg,
// which R_RecursiveWorldNode's plane tests read) right before it.
Site g_origin{"current view origin",
              {"current_view_origin", "F3 0F 11 05 ? ? ? ? F3 0F 10 40 04 F3 0F 11 05 ? ? ? ? F3 0F 10 40 08 53 F3 0F 11 05",
               0xE7665}};
// client.dll: the CullBox branch (engine->CullBox, IVEngineClient +0x84); the
// other one (r_PortalTestEnts) tests the portal frusta.
Site g_entities{"ExtractCulledRenderables CullBox",
                {"extract_culled_renderables", "8B 80 84 00 00 00 8D 57 F8 52 8D 57 EC 52 FF D0 84 C0 74 ? FF 4B 0C EB", 0xE2123},
                "not installed", 18};
Site g_water{"DoesViewPlaneIntersectWater", {"intersect_water", "74 ? 83 8E ? ? ? ? 30 EB ? 80 7B 08 00", 0x1EB163}};
// R_BuildWorldLists: "mov edx,[host_state.worldbrush]; mov edx,[edx+0x54]
// (nodes); mov ecx,ebx; call R_RecursiveWorldNode".
constexpr char kWorldBrushLoad[] = "8B 15";
constexpr uintptr_t kWorldBrushLoadAt = 6;
constexpr char kPreWorldNode[] = "8B 52 54 8B CB E8";
// R_RecursiveWorldNode's "cmp ecx,[r_visframecount]" at +0x29, its operand at
// +0x2B.
constexpr char kWalkVisframe[] = "3B 0D";
constexpr uintptr_t kWalkVisframeAt = 0x29, kWalkVisframeOperand = 0x2B;
constexpr uintptr_t kOriginOperands[3] = {4, 17, 31};
// worldbrushdata_t: L4D2's 4 bytes on (Mod_LoadLeafs and Mod_LoadNodes store
// the counts and arrays there).
constexpr size_t kNumLeafs = 0x10, kLeafs = 0x14, kNumNodes = 0x50, kNodes = 0x54;
#endif

Site *const kSites[] = {&g_cullNode01, &g_cullNodeFn, &g_cullReturn, &g_cullSkip,    &g_backface01,
                        &g_backface02, &g_drawLeaf,   &g_brushModel, &g_dispInfo,    &g_preWorldNode,
                        &g_visframe,   &g_origin,     &g_entities,   &g_water};

struct Statistics {
  std::atomic<uint64_t> nearKept{0}, frustumKept{0}, frustumCulled{0}, stockViews{0}, walks{0}, leavesForced{0};
} g_stats;
std::atomic<const char *> g_leafState{"not installed"};

template <typename T> T read(const uint8_t *p, size_t offset) {
  T value;
  std::memcpy(&value, p + offset, sizeof(T));
  return value;
}

// The target of a rel32 call or jump whose displacement is at `at`.
uintptr_t relTarget(const Module &module, uintptr_t displacementRva) {
  return displacementRva + 4 + uintptr_t(read<int32_t>(module.at<uint8_t>(0), displacementRva));
}

bool stockView() {
  const int view = viewId();
#ifdef PROXY_GAME_PORTAL2
  return view != kViewMain && view >= 0;
#else
  return view == kView3dSky || view == kViewMonitor;
#endif
}

// The node's box (VectorAligned m_vecCenter +0x10, m_vecHalfDiagonal +0x20)
// within `distance` of the view origin.
bool nearView(const uint8_t *node) {
  const float *center = reinterpret_cast<const float *>(node + 0x10);
  const float *half = reinterpret_cast<const float *>(node + 0x20);
  float d2 = 0;
  for (int i = 0; i < 3; ++i) {
    const float lo = center[i] - half[i], hi = center[i] + half[i], p = g_viewOrigin[i];
    const float d = p < lo ? lo - p : p > hi ? p - hi : 0.0f;
    d2 += d * d;
  }
  return d2 <= g_distance * g_distance;
}

// The world whose leaves were checked: worldbrushdata_t (kNumLeafs, kLeafs,
// kNumNodes, kNodes); mnode_t and mleaf_t are 0x40 bytes {contents, visframe
// +4, parent +8, area, flags, center +0x10, half diagonal +0x20, ..} (both
// games).
// The world is host_state's, one address for every map: a check is kept for
// the leaf and node arrays it saw (a map that was still loading, or the next
// one, has others).
struct CheckedWorld {
  const uint8_t *leafs, *nodes;
  int leafCount, nodeCount;
  bool operator==(const CheckedWorld &o) const {
    return leafs == o.leafs && nodes == o.nodes && leafCount == o.leafCount && nodeCount == o.nodeCount;
  }
};
CheckedWorld g_checkedWorld{};
bool g_worldUsable = false;

bool worldUsable(const uint8_t *world) {
  const int leafCount = read<int>(world, kNumLeafs), nodeCount = read<int>(world, kNumNodes);
  const uint8_t *leafs = read<uint8_t *>(world, kLeafs), *nodes = read<uint8_t *>(world, kNodes);
  const CheckedWorld checked{leafs, nodes, leafCount, nodeCount};
  if (checked == g_checkedWorld)
    return g_worldUsable;
  g_checkedWorld = checked;
  g_worldUsable = false;
  if (leafCount <= 0 || nodeCount <= 0 || leafCount > 0x100000 || nodeCount > 0x100000 || !leafs || !nodes ||
      read<int>(nodes, 0) != -1) {
    g_leafState = "world layout not as expected: no leaf forcing";
    return false;
  }
  const uint8_t *nodesEnd = nodes + size_t(nodeCount) * 0x40;
  for (int i = 0; i < leafCount && i < 64; ++i) {
    const uint8_t *leaf = leafs + size_t(i) * 0x40, *parent = read<uint8_t *>(leaf, 8);
    if (read<int>(leaf, 0) < 0 || parent < nodes || parent >= nodesEnd || (parent - nodes) % 0x40) {
      g_leafState = "world layout not as expected: no leaf forcing";
      return false;
    }
  }
  g_leafState = "ok";
  return g_worldUsable = true;
}

} // namespace
} // namespace proxy::x86

extern "C" {
uintptr_t openremix_x86_cullReturn = 0, openremix_x86_skipReturn = 0, openremix_x86_preWorldNodeReturn = 0;

// Instead of R_CullNode(node) in R_RecursiveWorldNode: nonzero culls.
[[gnu::force_align_arg_pointer]] int openremix_x86_cullNode(const uint8_t *node) {
  using namespace proxy::x86;
  if (stockView()) {
    ++g_stats.stockViews;
    return g_cullNode(node);
  }
  if (g_distance > 0 && nearView(node)) {
    ++g_stats.nearKept;
    return 0;
  }
  const bool culled = g_cullNode(node);
  ++(culled ? g_stats.frustumCulled : g_stats.frustumKept);
  return culled;
}

// Before R_RecursiveWorldNode(nodes): the leaves near the view join the
// current visframe, with their parents up to one that already has it.
[[gnu::force_align_arg_pointer]] void openremix_x86_preWorldNode() {
  using namespace proxy::x86;
  if (stockView() || g_distance <= 0)
    return;
  const uint8_t *world = *g_worldBrush;
  if (!world || !worldUsable(world))
    return;
  ++g_stats.walks;
  const int visframe = *g_visframecount;
  const int leafCount = read<int>(world, kNumLeafs), nodeCount = read<int>(world, kNumNodes);
  uint8_t *leafs = read<uint8_t *>(world, kLeafs), *nodes = read<uint8_t *>(world, kNodes);
  const uint8_t *nodesEnd = nodes + size_t(nodeCount) * 0x40;
  uint64_t forced = 0;
  for (int i = 0; i < leafCount; ++i) {
    uint8_t *leaf = leafs + size_t(i) * 0x40;
    if (!nearView(leaf))
      continue;
    ++forced;
    std::memcpy(leaf + 4, &visframe, 4);
    for (uint8_t *node = read<uint8_t *>(leaf, 8); node >= nodes && node < nodesEnd && read<int>(node, 4) != visframe;
         node = read<uint8_t *>(node, 8))
      std::memcpy(node + 4, &visframe, 4);
  }
  g_stats.leavesForced += forced;
}

void openremix_x86_cullNodeStub();
void openremix_x86_preWorldNodeStub();
}

// Replaces "call R_CullNode" (after "push ebx", the node): the engine's own
// exits after the test, as R_CullNode's answer would take them.
asm(R"(
  .text
  .globl _openremix_x86_cullNodeStub
_openremix_x86_cullNodeStub:
  .intel_syntax noprefix
  pushad
  push ebx
  call _openremix_x86_cullNode
  add esp, 4
  test eax, eax
  jz .Lopenremix_x86_visible
  popad
  add esp, 4
  jmp dword ptr [_openremix_x86_cullReturn]
.Lopenremix_x86_visible:
  popad
  add esp, 4
  jmp dword ptr [_openremix_x86_skipReturn]
  .att_syntax prefix
)");

// Replaces "mov edx,[eax+0x50]; mov ecx,ebx" (Portal 2: "mov edx,[edx+0x54];
// mov ecx,ebx") before "call R_RecursiveWorldNode".
#ifdef PROXY_GAME_PORTAL2
#define OPENREMIX_X86_NODES_LOAD "mov edx, [edx+0x54]"
#else
#define OPENREMIX_X86_NODES_LOAD "mov edx, [eax+0x50]"
#endif
asm(R"(
  .text
  .globl _openremix_x86_preWorldNodeStub
_openremix_x86_preWorldNodeStub:
  .intel_syntax noprefix
  pushad
  call _openremix_x86_preWorldNode
  popad
  )" OPENREMIX_X86_NODES_LOAD R"(
  mov ecx, ebx
  jmp dword ptr [_openremix_x86_preWorldNodeReturn]
  .att_syntax prefix
)");

namespace proxy::x86 {
namespace {

// Rewrites `n` bytes at the site's match + offset from `expected` (checked)
// to `patched`.
bool replace(const Module &module, Site &site, size_t offset, const char *expected, const uint8_t *patched,
             size_t n) {
  if (!site.pattern.bytes) {
    site.state = "not in this engine";
    return false;
  }
  if (!site.pattern.rva) {
    site.state = "pattern not found";
    return false;
  }
  if (!bytesAt(module, site.pattern.rva + offset, expected)) {
    site.state = "bytes differ";
    return false;
  }
  if (!writeCode(module.at<uint8_t>(site.pattern.rva + offset), patched, n)) {
    site.state = "cannot write";
    return false;
  }
  site.state = "ok";
  return true;
}

// R_RecursiveWorldNode's node test: "cmp eax,-1; jl visible; push ebx; call
// R_CullNode; add esp,4; test al,al; jne culled".
void installCullNode(const Module &engine) {
  Site &site = g_cullNode01;
  const uintptr_t at = site.pattern.rva;
#ifdef PROXY_GAME_PORTAL2
  // The culled exit: where the test's jne goes (g_cullReturn has no pattern).
  if (at && bytesAt(engine, at + 13, "0F 85")) {
    const uintptr_t exit = relTarget(engine, at + 15);
    if (bytesAt(engine, exit, "5F 5E 5B 8B E5 5D C3")) {
      g_cullReturn.pattern.rva = exit;
      g_cullReturn.state = "ok (from the node test's jump)";
    } else {
      g_cullReturn.state = "the node test's jump goes elsewhere";
    }
  }
#endif
  if (!at || !g_cullNodeFn.pattern.rva || !g_cullReturn.pattern.rva || !g_cullSkip.pattern.rva) {
    site.state = "a pattern was not found";
    return;
  }
  const uintptr_t visible = at + 2 + uintptr_t(int8_t(*engine.at<uint8_t>(at + 1)));
  const uintptr_t callee = relTarget(engine, at + 4);
  if (!bytesAt(engine, at + 8, "83 C4 04 84 C0 0F 85") || visible != g_cullSkip.pattern.rva ||
      callee != g_cullNodeFn.pattern.rva || relTarget(engine, at + 15) != g_cullReturn.pattern.rva ||
      !bytesAt(engine, visible, "83 3B 00 0F 8D")) {
    site.state = "the code around it is not as expected";
    return;
  }
  g_cullNode = reinterpret_cast<CullNodeFn>(engine.base + callee);
  openremix_x86_cullReturn = engine.base + g_cullReturn.pattern.rva;
  openremix_x86_skipReturn = engine.base + visible;
  const uint8_t jle = 0x7E;
  if (!writeJump(engine.at<uint8_t>(at + 3), reinterpret_cast<void *>(&openremix_x86_cullNodeStub), 5) ||
      !writeCode(engine.at<uint8_t>(at), &jle, 1)) {
    site.state = "cannot write";
    return;
  }
  site.state = "ok";
}

// R_BuildWorldLists: the world brush's nodes loaded (kWorldBrushLoad, its
// operand the 4 bytes before the match), then "call R_RecursiveWorldNode"
// (kPreWorldNode), whose visframe test (kWalkVisframe) names the visframe the
// walk enters.
void installLeafForcing(const Module &engine) {
  Site &site = g_preWorldNode;
  const uintptr_t at = site.pattern.rva;
  if (!at || !g_visframe.pattern.rva || !g_origin.pattern.rva) {
    site.state = "a pattern was not found";
    return;
  }
  const uintptr_t walk = relTarget(engine, at + 6);
  const uintptr_t visframe = read<uint32_t>(engine.at<uint8_t>(0), g_visframe.pattern.rva + 2);
  const uintptr_t origin = read<uint32_t>(engine.at<uint8_t>(0), g_origin.pattern.rva + kOriginOperands[0]);
  if (!bytesAt(engine, at - kWorldBrushLoadAt, kWorldBrushLoad) || !bytesAt(engine, at, kPreWorldNode) ||
      !bytesAt(engine, walk + kWalkVisframeAt, kWalkVisframe) ||
      read<uint32_t>(engine.at<uint8_t>(0), walk + kWalkVisframeOperand) != visframe ||
      !(g_cullNode01.pattern.rva > walk && g_cullNode01.pattern.rva - walk < 0x100) ||
      read<uint32_t>(engine.at<uint8_t>(0), g_origin.pattern.rva + kOriginOperands[1]) != origin + 4 ||
      read<uint32_t>(engine.at<uint8_t>(0), g_origin.pattern.rva + kOriginOperands[2]) != origin + 8 ||
      !engine.holds(reinterpret_cast<void *>(visframe)) || !engine.holds(reinterpret_cast<void *>(origin))) {
    site.state = "the code around it is not as expected";
    return;
  }
  g_worldBrush = reinterpret_cast<uint8_t *const *>(uintptr_t(read<uint32_t>(engine.at<uint8_t>(0), at - 4)));
  g_visframecount = reinterpret_cast<const int *>(visframe);
  g_viewOrigin = reinterpret_cast<const float *>(origin);
  g_visframe.state = g_origin.state = "ok";
  openremix_x86_preWorldNodeReturn = engine.base + at + 5;
  if (!writeJump(engine.at<uint8_t>(at), reinterpret_cast<void *>(&openremix_x86_preWorldNodeStub), 5)) {
    site.state = "cannot write";
    return;
  }
  site.state = "ok";
}

} // namespace

void installCull(const Module &engine, const Module &client) {
  if (!envFlag("ENGINE_NOCULL", true)) {
    g_status = "off (OPENREMIX_PROXY_ENGINE_NOCULL=0)";
    return;
  }
  g_distance = envFloat("NOCULL_DISTANCE", 600.0f);
  for (Site *site : kSites) {
    if (site->pattern.bytes)
      site->pattern.find(site == &g_entities || site == &g_water ? client : engine);
    else if (site != &g_cullReturn)
      site->state = "not in this engine";
  }
  // The view origin is needed for the distance rule.
  installLeafForcing(engine);
  if (g_viewOrigin)
    installCullNode(engine);
  else
    g_cullNode01.state = "no view origin";
  const uint8_t nop2[] = {0x90, 0x90}, nop6[] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90}, je = 0x74, jmp = 0xEB;
  // test [edx+ecx*4],ebx; je next  -> nops
  replace(engine, g_backface01, 0, "74", nop2, 2);
  // test ecx,0x20000 (SURFDRAW_NOCULL); jne draw -> je
  replace(engine, g_backface02, 0, "75", &je, 1);
  // comiss xmm1,xmm0; ja skip (plane normal test) -> nops
  replace(engine, g_drawLeaf, 0, "0F 87", nop6, 6);
  // cmp [bShadowDepth],0; je front-facing only -> nops
  replace(engine, g_brushModel, 0, "74", nop2, 2);
  // call Frustum_t::CullBox; test al,al; jne return -> nops
  replace(engine, g_dispInfo, 0, "75", nop2, 2);
  // call engine->CullBox; test al,al; je visible -> jmp
  replace(client, g_entities, g_entities.offset, "74", &jmp, 1);
  // call DoesViewPlaneIntersectWater; test al,al; je .. -> nops (both sides drawn)
  replace(client, g_water, 0, "74", nop2, 2);
  unsigned ok = 0, total = 0;
  for (Site *site : {&g_cullNode01, &g_preWorldNode, &g_backface01, &g_backface02, &g_drawLeaf, &g_brushModel,
                     &g_dispInfo, &g_entities, &g_water}) {
    if (!site->pattern.bytes)
      continue; // not in this engine (Portal 2's CDispInfo::Render)
    ++total;
    ok += std::strcmp(site->state.load(), "ok") == 0;
  }
  g_status = ok == total ? "installed" : ok ? "partly installed" : "nothing installed";
  diag::stage("%s: anti-culling %s (%u of %u sites, distance %.0f)", kGame, g_status.load(), ok, total, g_distance);
  for (Site *site : kSites)
    if (std::strcmp(site->state.load(), "ok") != 0 && site != &g_cullNodeFn && site != &g_cullReturn &&
        site != &g_cullSkip && site->pattern.bytes)
      diag::stage("%s:   %s: %s (pattern %s: %u matches)", kGame, site->name, site->state.load(),
                  site->pattern.name, site->pattern.count);
}

void dumpCull(FILE *out) {
  static ULONGLONG last = GetTickCount64();
  const ULONGLONG now = GetTickCount64();
  const double seconds = now > last ? double(now - last) / 1000 : 1;
  last = now;
  auto rate = [&](std::atomic<uint64_t> &counter) { return double(counter.exchange(0)) / seconds; };
  fprintf(out, "== %s anti-culling: %s | distance %.0f | leaf forcing: %s\n", kGame, g_status.load(), g_distance,
          g_leafState.load());
  for (const Site *site : kSites)
    fprintf(out, "   %s: %s (pattern %s: %u matches, first +0x%lX, expected +0x%lX)\n", site->name,
            site->state.load(), site->pattern.name, site->pattern.count, (unsigned long)site->pattern.rva,
            (unsigned long)site->pattern.staticRva);
  const double walks = rate(g_stats.walks);
  fprintf(out,
          "   per second: leaves kept near the view %.0f, by the frustum %.0f, culled %.0f, in stock views %.0f | "
          "world walks %.1f, leaves forced %.0f\n",
          rate(g_stats.nearKept), rate(g_stats.frustumKept), rate(g_stats.frustumCulled), rate(g_stats.stockViews),
          walks, rate(g_stats.leavesForced));
}

} // namespace proxy::x86
