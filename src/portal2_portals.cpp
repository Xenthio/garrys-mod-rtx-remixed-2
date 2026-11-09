// Portal 2: the portals as path-traced ray portals (portal2.h).
//
// The portals are C_Prop_Portal client entities (network class CProp_Portal,
// its table DT_Prop_Portal on DT_Portal_Base2D), found by their primary
// vtable (RTTI .?AVC_Prop_Portal@@) among VClientEntityList003's entities.
// Networked variables, from the receive tables (offsets in the client.dll of
// this port, as found when it was written, in brackets): m_ptOrigin [0x1014],
// m_qAbsAngle [0x1044], m_hLinkedPortal [0x106C], m_bIsPortal2 [0x1050],
// m_bActivated [0x34F0], m_fNetworkHalfWidth [0x34F4], m_fNetworkHalfHeight
// [0x34F8] (DT_Portal_Base2D), m_hFiredByPlayer (DT_Prop_Portal). Client-only
// fields, used only while the networked ones sit where they were found:
//   m_fStaticAmount +0x3510, m_fSecondaryStaticAmount +0x3514,
//   m_fOpenAmount +0x3518: C_Prop_Portal::GetToolRecordingState copies them
//     into PortalRecordingState_t (+8, +0xC, +0x10, beside its "Prop_Portal"
//     type name at +0x1C); the PortalOpenAmount material proxy reads +0x3514
//     of the renderable it binds (the entity + 4); a new placement sets +0x3518
//     to 0.
//   m_matrixThisToLinked +0xDBC (a VMatrix, C_Portal_Base2D's prediction
//     description: m_vForward +0x1020, m_plane_Origin +0x1074 beside it):
//     only compared with the transform computed here (statistics).
//
// A portal's frame: origin m_ptOrigin; axes from m_qAbsAngle (Source's
// AngleVectors: forward, left = -right, up), the columns of the instance
// transform T. The mesh lies in local x = kOffset (just in front of the wall),
// an oval polygon of half width m_fNetworkHalfWidth along local y and half
// height m_fNetworkHalfHeight along local z (Portal 2's PORTAL_HALF_WIDTH 32,
// PORTAL_HALF_HEIGHT 54 when not networked), facing +X. The runtime teleports
// p -> T_B diag(-1,-1,1) T_A^-1 p (Source's UpdatePortalTransformationMatrix:
// linked-to-world x 180 degrees about up x world-to-this).
//
// Indices: a pair takes a slot (0..3; ray portal index = slot * 2 +
// m_bIsPortal2, the partner index ^ 1). The pair is named by the player who
// fired it (m_hFiredByPlayer: one pair per player, Portal 2's linkage groups in
// co-op), else by its primary portal's entity (map-placed portals). Pairs past
// four slots, and a second portal claiming an index, are left out (counted).
//
// Opening: m_fOpenAmount o rises from 0 to 1 as a portal opens. The engine's
// hole is the stencil PortalRefract $Stage 1 draws over the portal's quad
// (models/portals/portal1.mdl: 2 half width by 2 half height, 1 unit in front
// of the origin): where |2 (1.075 uv - 0.0375) - 1| <= smoothstep(o)^2, an
// oval of smoothstep(o)^2 / 1.075 the quad's size. The oval is scaled so (its
// vertices, not the transform: the frame stays rigid), and not submitted below
// 5% (OPENREMIX_PROXY_PORTAL_OPENING=0: always open).
// A lone portal (no activated partner) is submitted too: the runtime shows its
// material, the portal's colour with a dim glow, under the rim's swirl.
//
// The game's portal effects, drawn by the runtime instead of the engine:
//  * the rim (PortalRefract $Stage 2, materials portalstaticoverlay_1/_2:
//    remixapi_MaterialPortalRimEXT, the shader's maths): an elliptic disc over
//    the engine's quad (the part of it the effect covers), 1 unit in front of
//    the origin, its noise noise-blur-256x256, its ramp portal-blue-color /
//    portal-orange-color, colour scale 4; the opening and the static
//    (m_fStaticAmount, 1 for a lone portal) in its vertex colours. Two
//    instances: a raster-only overlay (source alpha over, depth-tested, as the
//    engine draws it), and a traced reflection-only copy whose rings are cut
//    to where the coverage (with the noise at its mean) is at least 1/2,
//    0.15 units in front of the origin (kTracedRimOffset), which lights the
//    scene and shows in reflections and through other portals. OPENREMIX_PROXY_PORTAL_RIM: its emission scale (default 1;
//    0 no rim of the proxy's: the engine's overlays as raster overlays, as
//    before). OPENREMIX_PROXY_PORTAL_ENGINE_RIMS=1: the engine's rims replayed
//    with its shaders instead of the overlay (the traced copy stays).
//  * the ghost (PortalStaticOverlay $ghostoverlay, portalstaticoverlay_1_noz /
//    _2_noz): the quad 2 units in front of the origin with dummy-blue /
//    dummy-orange (the portal's outline and corner brackets), drawn where
//    something is nearer than it (depth test GREATER: the portal seen through
//    a wall or an object in front of it), premultiplied (ONE, ONE - source
//    alpha): colour 0.3 f T.rgb, alpha f T.a, f = 1 seen from behind, else
//    saturate((distance^2 - 120^2) / (240^2 - 120^2)) (gone within 120
//    units of its front). OPENREMIX_PROXY_PORTAL_GHOSTS=0: none (nor with
//    OPENREMIX_PROXY_PORTAL_RIM=0, when the engine's are drawn).
//
// OPENREMIX_PROXY_PORTAL_OFFSET: the mesh's distance in front of the portal's
// origin (default 0.05 units). OPENREMIX_PROXY_PORTAL_EMISSIVE: the lone
// portal's glow (default 1, 0 none).
// The library loader (remixapi_lib_*) is remix_output's.
#define REMIX_WINAPI_NO_LIBRARY_LOADER
#include <remix/remix_c.h>
#include <remix/remix_native_ext.h>

#include "portal2.h"

#include "bsp_world.h"
#include "diag.h"
#include "game.h"
#include "x86_engine.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace proxy::portal2 {
namespace {

using x86::Module;

constexpr int kSlots = 4, kIndices = kSlots * 2;
// Portal 2's PORTAL_HALF_WIDTH, PORTAL_HALF_HEIGHT.
constexpr float kHalfWidth = 32, kHalfHeight = 54;
constexpr int kSegments = 32;

std::atomic<const char *> g_status{"not installed"};
void *g_entityList = nullptr; // VClientEntityList003: 3 GetClientEntity(int), 6 GetHighestEntityIndex()
void *g_portalVtable = nullptr;

// Networked (receive tables); -1: not found.
struct NetVars {
  int origin = -1, angles = -1, linked = -1, isPortal2 = -1, activated = -1, halfWidth = -1, halfHeight = -1,
      firedBy = -1;
} g_vars;
// Where they were when the client-only offsets below were found.
constexpr NetVars kExpected = {0x1014, 0x1044, 0x106C, 0x1050, 0x34F0, 0x34F4, 0x34F8, -1};
constexpr int kStaticAmount = 0x3510, kOpenAmount = 0x3518, kThisToLinked = 0xDBC;
bool g_clientFields = false; // the networked layout is the expected one: the client-only offsets hold

// One portal as sampled (main thread) and as submitted (render thread).
struct Portal {
  int entity = 0;
  int index = -1;             // ray portal index
  bool partner = false;       // its partner (index ^ 1) is submitted too
  float transform[3][4] = {}; // the portal's frame (rigid)
  float halfWidth = kHalfWidth, halfHeight = kHalfHeight;
  float open = 1;             // m_fOpenAmount (quantized), 1 when open
  float stat = 0;             // the rim's static: m_fStaticAmount, 1 for a lone portal (quantized)
  float scale = 1 / 1.075f;   // the hole's size in the quad's: smoothstep(open)^2 / 1.075
  bool hole = true;           // the hole is submitted (scale at least 5%)
};
struct Snapshot {
  std::vector<Portal> portals;
};
bool samePortals(const std::vector<Portal> &a, const std::vector<Portal> &b) {
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (a[i].entity != b[i].entity || a[i].index != b[i].index || a[i].partner != b[i].partner ||
        std::memcmp(a[i].transform, b[i].transform, sizeof(a[i].transform)) != 0 ||
        a[i].halfWidth != b[i].halfWidth || a[i].halfHeight != b[i].halfHeight || a[i].scale != b[i].scale ||
        a[i].open != b[i].open || a[i].stat != b[i].stat || a[i].hole != b[i].hole)
      return false;
  return true;
}

// Main thread to render thread (engine_lights' pattern).
std::mutex g_lock;
std::deque<std::pair<uint32_t, std::shared_ptr<const Snapshot>>> g_pending; // under g_lock
std::shared_ptr<const Snapshot> g_current;                                  // under g_lock
std::shared_ptr<const Snapshot> g_published;                                // main thread
uint32_t g_sequence = 0;                                                    // main thread

// Main thread: the pairs' slots.
struct Slot {
  uint32_t key = 0;
  bool used = false, seen = false;
};
Slot g_slots[kSlots];

// Render thread: the materials (one per index) and meshes (per index and size).
remixapi_MaterialHandle g_materials[kIndices] = {};
bool g_materialFailed[kIndices] = {};
struct Mesh {
  remixapi_MeshHandle handle = nullptr;
  uint64_t key = 0;
};
Mesh g_meshes[kIndices];
// The game's portal effects (rimMaterialFor, ghostMaterialFor: one per
// colour; the meshes per index, made again as the portal changes).
remixapi_MaterialHandle g_rimMaterials[2][2] = {}, g_ghostMaterials[2] = {}; // rims: [traced][colour]
bool g_rimMaterialFailed[2][2] = {}, g_ghostMaterialFailed[2] = {};
Mesh g_rimMeshes[kIndices], g_tracedRimMeshes[kIndices], g_ghostMeshes[kIndices];
// Their textures, read from the game's VTFs once (render thread): the rim's
// noise and ramps, the ghosts' images (per colour), as runtime textures.
struct EffectTextures {
  bool tried = false;
  std::string failure; // empty: all made
  remixapi_TextureHandle handles[5] = {};
  uint64_t noise = 0, ramp[2] = {}, ghost[2] = {};
} g_effectTextures;

struct Statistics {
  std::atomic<uint64_t> samples{0}, found{0}, activated{0}, submitted{0}, pairs{0}, lone{0}, opening{0}, overSlots{0},
      conflicts{0}, published{0}, drawn{0}, meshesBuilt{0}, failed{0};
  // Per second: rim overlays, traced rim copies and ghosts drawn.
  std::atomic<uint64_t> rims{0}, tracedRims{0}, ghosts{0};
  // Since launch: sets announced, their markers received, submit() calls and
  // those without a set yet.
  std::atomic<uint64_t> announced{0}, received{0}, submits{0}, submitsEmpty{0};
  // m_matrixThisToLinked against the transform computed here (linked pairs):
  // the largest difference seen in the rotation part and in the translation
  // (units), and how many pairs were compared.
  std::atomic<uint64_t> compared{0};
  float rotationError = 0, translationError = 0; // main thread writes, statistics read (a stale value is fine)
} g_stats;
std::mutex g_textLock;
std::string g_lastSet = "none"; // under g_textLock: the last sampled set, for the statistics

template <typename T> T read(const uint8_t *p, int offset) {
  T value;
  std::memcpy(&value, p + offset, sizeof(T));
  return value;
}

bool finite3(const float *v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

// Source's AngleVectors (pitch, yaw, roll in degrees).
void angleVectors(const float *angles, float *forward, float *right, float *up) {
  constexpr float kRad = 3.14159265358979f / 180.0f;
  const float sp = std::sin(angles[0] * kRad), cp = std::cos(angles[0] * kRad);
  const float sy = std::sin(angles[1] * kRad), cy = std::cos(angles[1] * kRad);
  const float sr = std::sin(angles[2] * kRad), cr = std::cos(angles[2] * kRad);
  forward[0] = cp * cy, forward[1] = cp * sy, forward[2] = -sp;
  right[0] = -sr * sp * cy + cr * sy, right[1] = -sr * sp * sy - cr * cy, right[2] = -sr * cp;
  up[0] = cr * sp * cy + sr * sy, up[1] = cr * sp * sy - sr * cy, up[2] = cr * cp;
}

// The portal's frame: columns forward, left, up, origin.
void frame(const float *origin, const float *angles, float out[3][4]) {
  float forward[3], right[3], up[3];
  angleVectors(angles, forward, right, up);
  for (int r = 0; r < 3; ++r) {
    out[r][0] = forward[r];
    out[r][1] = -right[r];
    out[r][2] = up[r];
    out[r][3] = origin[r];
  }
}

// T_B diag(-1,-1,1) T_A^-1 (rigid frames), as a 3x4.
void thisToLinked(const float a[3][4], const float b[3][4], float out[3][4]) {
  // R_A^-1 = R_A^T; t_A^-1 = -R_A^T t_A. M = R_B D R_A^T, t = R_B D (-R_A^T t_A) + t_B.
  float bd[3][3];
  for (int r = 0; r < 3; ++r) {
    bd[r][0] = -b[r][0];
    bd[r][1] = -b[r][1];
    bd[r][2] = b[r][2];
  }
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      float s = 0;
      for (int k = 0; k < 3; ++k)
        s += bd[r][k] * a[c][k]; // (R_A^T)[k][c] = a[c][k]
      out[r][c] = s;
    }
    float t = b[r][3];
    for (int c = 0; c < 3; ++c)
      t -= out[r][c] * a[c][3];
    out[r][3] = t;
  }
}

// The slot of a pair (main thread); -1 when all are taken.
int slotFor(uint32_t key) {
  for (int s = 0; s < kSlots; ++s)
    if (g_slots[s].used && g_slots[s].key == key) {
      g_slots[s].seen = true;
      return s;
    }
  for (int s = 0; s < kSlots; ++s)
    if (!g_slots[s].used) {
      g_slots[s] = {key, true, true};
      return s;
    }
  return -1;
}

void announce(uint32_t sequence) {
  char text[48];
  std::snprintf(text, sizeof(text), "openremix:portals %u", sequence);
  x86::marker(text);
  ++g_stats.announced;
}

// A portal's colour: Portal 2's blue and orange, the bright ends of the rim
// shader's ramps (models/portals/portal-blue-color, portal-orange-color:
// 256 x 1, black to this). The closed look and the rim.
void colorOf(int index, float out[3]) {
  static const float kColors[2][3] = {{0.008f, 0.435f, 0.804f}, {0.965f, 0.494f, 0.0f}};
  std::memcpy(out, kColors[index & 1], sizeof(kColors[0]));
}

remixapi_MaterialHandle materialFor(const remixapi_Interface &api, int index) {
  if (g_materials[index] || g_materialFailed[index])
    return g_materials[index];
  static const float emissive = std::max(0.0f, x86::envFloat("PORTAL_EMISSIVE", 1.0f));
  float color[3];
  colorOf(index, color);
  remixapi_MaterialInfoPortalEXT portal = {};
  portal.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_PORTAL_EXT;
  portal.rayPortalIndex = uint8_t(index);
  portal.rotationSpeed = 0;
  remixapi_MaterialInfoOpaqueEXT opaque = {};
  opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
  opaque.pNext = &portal;
  opaque.albedoConstant = {color[0], color[1], color[2]};
  opaque.opacityConstant = 1;
  opaque.roughnessConstant = 0.6f;
  opaque.alphaTestType = 7; // always
  remixapi_MaterialInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
  info.pNext = &opaque;
  info.hash = 0x706f7274616c0000ull + uint64_t(index); // "portal"
  if (emissive > 0) {
    info.emissiveIntensity = emissive;
    info.emissiveColorConstant = {color[0], color[1], color[2]};
  }
  info.filterMode = 1;
  info.wrapModeU = info.wrapModeV = 1;
  if (api.CreateMaterial(&info, &g_materials[index]) != REMIXAPI_ERROR_CODE_SUCCESS || !g_materials[index]) {
    g_materials[index] = nullptr;
    g_materialFailed[index] = true;
    ++g_stats.failed;
    diag::stage("Portal 2: portals: the material of ray portal %d was refused", index);
  }
  return g_materials[index];
}

// The oval for one portal (local frame, see above), its material on its surface.
remixapi_MeshHandle meshFor(const remixapi_Interface &api, const Portal &p, remixapi_MaterialHandle material) {
  static const float offset = x86::envFloat("PORTAL_OFFSET", 0.05f);
  const float halfWidth = p.halfWidth * p.scale, halfHeight = p.halfHeight * p.scale;
  struct {
    float halfWidth, halfHeight, offset;
    int32_t index;
  } keyData = {halfWidth, halfHeight, offset, p.index};
  uint64_t key = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < sizeof(keyData); ++i)
    key = (key ^ reinterpret_cast<const uint8_t *>(&keyData)[i]) * 0x100000001b3ull;
  Mesh &m = g_meshes[p.index];
  if (m.handle && m.key == key)
    return m.handle;
  std::vector<remixapi_HardcodedVertex> vertices(kSegments + 1);
  std::vector<uint32_t> indices;
  auto vertex = [&](remixapi_HardcodedVertex &v, float y, float z) {
    v = {};
    v.position[0] = offset;
    v.position[1] = y;
    v.position[2] = z;
    v.normal[0] = 1;
    v.texcoord[0] = 0.5f - 0.5f * y / halfWidth; // +y is the portal's left: u grows to its right
    v.texcoord[1] = 0.5f - 0.5f * z / halfHeight;
    v.color = 0xffffffffu;
  };
  vertex(vertices[0], 0, 0);
  for (int i = 0; i < kSegments; ++i) {
    const float t = 2 * 3.14159265358979f * float(i) / kSegments;
    vertex(vertices[1 + i], halfWidth * std::cos(t), halfHeight * std::sin(t));
    // Counter-clockwise seen from +X: (c, a, b) with b at the larger angle.
    indices.insert(indices.end(), {0u, uint32_t(1 + i), uint32_t(1 + (i + 1) % kSegments)});
  }
  remixapi_MeshInfoSurfaceTriangles surface = {};
  surface.vertices_values = vertices.data();
  surface.vertices_count = vertices.size();
  surface.indices_values = indices.data();
  surface.indices_count = indices.size();
  surface.material = material;
  remixapi_MeshInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
  info.hash = key | 1;
  info.surfaces_values = &surface;
  info.surfaces_count = 1;
  remixapi_MeshHandle handle = nullptr;
  if (api.CreateMesh(&info, &handle) != REMIXAPI_ERROR_CODE_SUCCESS || !handle) {
    ++g_stats.failed;
    return m.handle; // the last one, if any
  }
  if (m.handle)
    api.DestroyMesh(m.handle);
  m.handle = handle;
  m.key = key;
  ++g_stats.meshesBuilt;
  return handle;
}

// The rim's emission scale (OPENREMIX_PROXY_PORTAL_RIM; 0: no rim of the
// proxy's, the engine's own portal overlays drawn as raster overlays as
// before).
float rimIntensity() {
  static const float v = std::max(0.0f, x86::envFloat("PORTAL_RIM", 1.0f));
  return v;
}
// The ghosts (OPENREMIX_PROXY_PORTAL_GHOSTS, on by default), while the proxy
// draws the rims (else the engine's ghosts are raster overlays again).
bool ghosts() {
  static const bool on = x86::envFlag("PORTAL_GHOSTS", true) && rimIntensity() > 0;
  return on;
}

// Where the effects lie in front of the portal's origin: the engine's quad
// (portal1.mdl's vertices at -1 along its normal), its ghost a unit further
// (PortalStaticOverlay's ghost pushes it out by the normal).
constexpr float kRimOffset = 1.0f, kGhostOffset = 2.0f;
// The rim's traced copy lies nearer the opening, between the ray portal's
// mesh (OPENREMIX_PROXY_PORTAL_OFFSET, 0.05) and where rays leaving the
// partner start (the runtime's rtx.native.pathtrace.rayPortalExitOffset,
// 0.25): rays met near the opening's edge see it before the portal, while
// rays coming out of a portal start past its own rim (at 1 unit they met it
// from behind, a ring of the other colour inside every view).
constexpr float kTracedRimOffset = 0.15f;
// PortalRefract's quad maps uv to c = 1.075 uv - 0.0375: the effect's extent,
// |2c - 1| = 1.075 at the quad's edges.
constexpr float kQuadReach = 1.075f;

uint64_t nameHash(const char *name) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (const char *c = name; *c; ++c)
    h = (h ^ uint8_t(*c)) * 0x100000001b3ull;
  return h | 1;
}

// The effects' textures (render thread, once): false while any is missing.
bool effectTextures(const remixapi_Interface &api) {
  auto &t = g_effectTextures;
  if (t.tried)
    return t.failure.empty();
  t.tried = true;
  struct Source {
    const char *name;
    bool srgb, mips;
    uint64_t *hash;
  } sources[] = {{"models/portals/noise-blur-256x256", false, true, &t.noise},
                 {"models/portals/portal-blue-color", true, false, &t.ramp[0]},
                 {"models/portals/portal-orange-color", true, false, &t.ramp[1]},
                 {"models/portals/dummy-blue", true, true, &t.ghost[0]},
                 {"models/portals/dummy-orange", true, true, &t.ghost[1]}};
  for (unsigned i = 0; i < 5; ++i) {
    const Source &source = sources[i];
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;
    if (!bsp::textureRgba(source.name, width, height, rgba)) {
      t.failure = std::string(source.name) + ": not read";
      break;
    }
    remixapi_TextureInfoGenerateMipsEXT mips = {};
    mips.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_GENERATE_MIPS_EXT;
    mips.generate = 1;
    remixapi_TextureInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
    info.pNext = source.mips ? &mips : nullptr;
    info.hash = nameHash(source.name) ^ 0x706f7274666f7800ull; // "portfx"
    info.width = width;
    info.height = height;
    info.depth = info.mipLevels = 1;
    info.format = source.srgb ? REMIXAPI_FORMAT_R8G8B8A8_SRGB : REMIXAPI_FORMAT_R8G8B8A8_UNORM;
    info.data = rgba.data();
    info.dataSize = rgba.size();
    if (api.CreateTexture(&info, &t.handles[i]) != REMIXAPI_ERROR_CODE_SUCCESS || !t.handles[i]) {
      t.handles[i] = nullptr;
      t.failure = std::string(source.name) + ": refused by the runtime";
      break;
    }
    *source.hash = info.hash;
  }
  if (!t.failure.empty())
    diag::stage("Portal 2: portals: the effects' textures: %s", t.failure.c_str());
  return t.failure.empty();
}

// The rim's material (per colour): PortalRefract $Stage 2 as
// remixapi_MaterialPortalRimEXT. The overlay's emits the shader's colour as
// it is (the raster backend draws that), the traced copy's that scaled by
// rimIntensity (the light it gives).
remixapi_MaterialHandle rimMaterialFor(const remixapi_Interface &api, int color, bool traced) {
  remixapi_MaterialHandle &material = g_rimMaterials[traced][color];
  bool &failed = g_rimMaterialFailed[traced][color];
  if (material || failed)
    return material;
  const auto &t = g_effectTextures;
  wchar_t noise[32], ramp[32];
  std::swprintf(noise, 32, L"0x%llx", static_cast<unsigned long long>(t.noise));
  std::swprintf(ramp, 32, L"0x%llx", static_cast<unsigned long long>(t.ramp[color]));
  remixapi_MaterialPortalRimEXT rim = {};
  rim.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_PORTAL_RIM_EXT;
  rim.colorTexture = ramp;
  rim.colorScale = 4.0f; // $PortalColorScale
  remixapi_MaterialInfoOpaqueEXT opaque = {};
  opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
  opaque.pNext = &rim;
  opaque.albedoConstant = {1, 1, 1};
  opaque.opacityConstant = 1;
  opaque.roughnessConstant = 1;
  opaque.alphaTestType = 7; // always
  remixapi_MaterialInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
  info.pNext = &opaque;
  info.hash = 0x706f7274616c7200ull + uint64_t(color) + (traced ? 2 : 0); // "portalr"
  info.albedoTexture = noise;
  info.emissiveIntensity = traced ? rimIntensity() : 1;
  info.emissiveColorConstant = {1, 1, 1};
  info.filterMode = 1;
  info.wrapModeU = info.wrapModeV = 1;
  if (api.CreateMaterial(&info, &material) != REMIXAPI_ERROR_CODE_SUCCESS || !material) {
    material = nullptr;
    failed = true;
    ++g_stats.failed;
    diag::stage("Portal 2: portals: the rim material of colour %d was refused", color);
  }
  return material;
}

// The ghost's material (per colour): the outline and brackets image, clamped.
// Unlit: its instances are raster overlays, coloured by their texture stages.
remixapi_MaterialHandle ghostMaterialFor(const remixapi_Interface &api, int color) {
  if (g_ghostMaterials[color] || g_ghostMaterialFailed[color])
    return g_ghostMaterials[color];
  wchar_t image[32];
  std::swprintf(image, 32, L"0x%llx", static_cast<unsigned long long>(g_effectTextures.ghost[color]));
  remixapi_MaterialInfoOpaqueEXT opaque = {};
  opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
  opaque.albedoConstant = {1, 1, 1};
  opaque.opacityConstant = 1;
  opaque.roughnessConstant = 1;
  opaque.alphaTestType = 7; // always
  remixapi_MaterialInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
  info.pNext = &opaque;
  info.hash = 0x706f727467680000ull + uint64_t(color); // "portgh"
  info.albedoTexture = image;
  info.filterMode = 1;
  info.wrapModeU = info.wrapModeV = 0; // clamp
  if (api.CreateMaterial(&info, &g_ghostMaterials[color]) != REMIXAPI_ERROR_CODE_SUCCESS || !g_ghostMaterials[color]) {
    g_ghostMaterials[color] = nullptr;
    g_ghostMaterialFailed[color] = true;
    ++g_stats.failed;
    diag::stage("Portal 2: portals: the ghost material of colour %d was refused", color);
  }
  return g_ghostMaterials[color];
}

// SolidEnergy (solidEnergyMaterial): the light bridge's textures and its
// materials by output intensity (render thread).
struct EnergyTextures {
  bool tried = false;
  std::string failure; // empty: all made
  remixapi_TextureHandle handles[3] = {};
  uint64_t hashes[3] = {}; // base, detail 1, detail 2
} g_energyTextures;
std::map<uint32_t, remixapi_MaterialHandle> g_energyMaterials; // by the intensity's bits (null: refused)
std::atomic<uint64_t> g_energyRefused{0};

bool energyTextures(const remixapi_Interface &api) {
  auto &t = g_energyTextures;
  if (t.tried)
    return t.failure.empty();
  t.tried = true;
  const char *names[3] = {"effects/projected_wall", "effects/projected_wall_draw", "effects/projected_wall_noise"};
  for (unsigned i = 0; i < 3; ++i) {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;
    if (!bsp::textureRgba(names[i], width, height, rgba)) {
      t.failure = std::string(names[i]) + ": not read";
      break;
    }
    remixapi_TextureInfoGenerateMipsEXT mips = {};
    mips.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_GENERATE_MIPS_EXT;
    mips.generate = 1;
    remixapi_TextureInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
    info.pNext = &mips;
    info.hash = nameHash(names[i]) ^ 0x656e657267790000ull; // "energy"
    info.width = width;
    info.height = height;
    info.depth = info.mipLevels = 1;
    info.format = REMIXAPI_FORMAT_R8G8B8A8_SRGB;
    info.data = rgba.data();
    info.dataSize = rgba.size();
    if (api.CreateTexture(&info, &t.handles[i]) != REMIXAPI_ERROR_CODE_SUCCESS || !t.handles[i]) {
      t.handles[i] = nullptr;
      t.failure = std::string(names[i]) + ": refused by the runtime";
      break;
    }
    t.hashes[i] = info.hash;
  }
  if (!t.failure.empty())
    diag::stage("Portal 2: light bridges: the textures: %s", t.failure.c_str());
  return t.failure.empty();
}

// A point of the engine's quad (2 half width by 2 half height) in the
// portal's frame: local y its left, z up; its texture coordinate as the
// model's (u to the portal's left, v down).
void quadVertex(remixapi_HardcodedVertex &v, const Portal &p, float x, float y, float z, uint32_t color) {
  v = {};
  v.position[0] = x;
  v.position[1] = y;
  v.position[2] = z;
  v.normal[0] = 1;
  v.texcoord[0] = 0.5f + 0.5f * y / p.halfWidth;
  v.texcoord[1] = 0.5f - 0.5f * z / p.halfHeight;
  v.color = color;
}

uint64_t meshKey(const void *data, size_t size, uint64_t seed) {
  uint64_t key = seed;
  for (size_t i = 0; i < size; ++i)
    key = (key ^ static_cast<const uint8_t *>(data)[i]) * 0x100000001b3ull;
  return key;
}

remixapi_MeshHandle makeMesh(const remixapi_Interface &api, Mesh &m, uint64_t key,
                             const std::vector<remixapi_HardcodedVertex> &vertices,
                             const std::vector<uint32_t> &indices, remixapi_MaterialHandle material) {
  remixapi_MeshInfoSurfaceTriangles surface = {};
  surface.vertices_values = vertices.data();
  surface.vertices_count = vertices.size();
  surface.indices_values = indices.data();
  surface.indices_count = indices.size();
  surface.material = material;
  remixapi_MeshInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
  info.hash = key | 1;
  info.surfaces_values = &surface;
  info.surfaces_count = 1;
  remixapi_MeshHandle handle = nullptr;
  if (api.CreateMesh(&info, &handle) != REMIXAPI_ERROR_CODE_SUCCESS || !handle) {
    ++g_stats.failed;
    return m.handle; // the last one, if any
  }
  if (m.handle)
    api.DestroyMesh(m.handle);
  m.handle = handle;
  m.key = key;
  ++g_stats.meshesBuilt;
  return handle;
}

// The rim's noiseless coverage at d = |2c - 1| (remixapi_MaterialPortalRimEXT
// with the noise term at its mean): where its traced copy is cut.
float rimCoverage(float d, float open, float stat) {
  auto smooth = [](float x) { return x * x * (3 - 2 * x); };
  auto saturate = [](float x) { return std::clamp(x, 0.0f, 1.0f); };
  constexpr float h = 0.53f;
  const float k = smooth(open), r = k * k;
  const bool inside = d <= r;
  const float e = inside ? saturate((d - r + 0.3f) / 0.3f) : 1 - saturate((d - r) * 13.3333f);
  const float edge = e * std::max(saturate(2.5f * k), stat);
  const float p = 1 - smooth(saturate((h - edge + 0.875f) / 1.75f));
  const float q = 1 - smooth(saturate((1 - stat) / h));
  return saturate(1.5f * saturate(edge * p + (inside ? q : 0.0f)));
}

// Elliptic rings over the engine's quad in the plane local x = x, between
// d = from and d = to (|2c - 1|, kQuadReach at the quad's edges; from 0: a
// disc), at most `step` apart, facing +X, counter-clockwise seen from +X as
// the opening.
void ringMesh(const Portal &p, float x, float from, float to, float step, uint32_t color,
              std::vector<remixapi_HardcodedVertex> &vertices, std::vector<uint32_t> &indices) {
  constexpr int kSegments = 48;
  const int rings = std::max(1, int(std::ceil((to - from) / step)));
  const uint32_t first = uint32_t(vertices.size());
  auto ring = [&](float d) {
    for (int i = 0; i < kSegments; ++i) {
      const float t = 2 * 3.14159265358979f * float(i) / kSegments;
      vertices.emplace_back();
      quadVertex(vertices.back(), p, x, d / kQuadReach * p.halfWidth * std::cos(t),
                 d / kQuadReach * p.halfHeight * std::sin(t), color);
    }
  };
  auto at = [&](int r, int i) { return first + uint32_t(r * kSegments + i % kSegments); };
  if (from <= 0) {
    vertices.emplace_back();
    quadVertex(vertices.back(), p, x, 0, 0, color);
    const uint32_t centre = first;
    for (int r = 1; r <= rings; ++r)
      ring(to * float(r) / rings);
    // Rings r = 1.. sit at at(r - 1, i) + 1 (after the centre).
    for (int i = 0; i < kSegments; ++i) {
      indices.insert(indices.end(), {centre, at(0, i) + 1, at(0, i + 1) + 1});
      for (int r = 0; r + 1 < rings; ++r)
        indices.insert(indices.end(), {at(r, i) + 1, at(r + 1, i) + 1, at(r + 1, i + 1) + 1, at(r, i) + 1,
                                       at(r + 1, i + 1) + 1, at(r, i + 1) + 1});
    }
    return;
  }
  for (int r = 0; r <= rings; ++r)
    ring(from + (to - from) * float(r) / rings);
  for (int i = 0; i < kSegments; ++i)
    for (int r = 0; r < rings; ++r)
      indices.insert(indices.end(),
                     {at(r, i), at(r + 1, i), at(r + 1, i + 1), at(r, i), at(r + 1, i + 1), at(r, i + 1)});
}

// The rim: the part of the engine's quad the effect can cover (the elliptic
// disc d <= 1.075), as rings. traced: its copy for rays, cut to where the
// coverage is at least 1/2 (rings at most 0.03 apart in d, 0.08 over a lone
// portal's filled disc: the runtime estimates its light per triangle). The opening (red) and the static
// (green) in the vertex colours. Null (traced): nothing is covered.
remixapi_MeshHandle rimMeshFor(const remixapi_Interface &api, const Portal &sampled, remixapi_MaterialHandle material,
                               bool traced) {
  // The traced copy follows the opening and the static in steps of 1/8 (the
  // overlay in the sample's 1/32): each new one is a new emitter, its light
  // estimated whole by the runtime and its light tree's dynamic part rebuilt.
  Portal p = sampled;
  if (traced) {
    p.open = std::round(p.open * 8) / 8;
    p.stat = std::round(p.stat * 8) / 8;
  }
  const uint32_t color = 0xff000000u | uint32_t(std::lround(p.open * 255)) << 16 |
                         uint32_t(std::lround(p.stat * 255)) << 8;
  struct {
    float halfWidth, halfHeight;
    uint32_t color;
    int32_t index, traced;
  } keyData = {p.halfWidth, p.halfHeight, color, p.index, traced};
  const uint64_t key = meshKey(&keyData, sizeof(keyData), 0x84222325cbf29ce4ull);
  Mesh &m = (traced ? g_tracedRimMeshes : g_rimMeshes)[p.index];
  if (m.key == key)
    return m.handle;
  std::vector<remixapi_HardcodedVertex> vertices;
  std::vector<uint32_t> indices;
  if (!traced) {
    ringMesh(p, kRimOffset, 0, kQuadReach, kQuadReach / 16, color, vertices, indices);
  } else {
    // The intervals of d (sampled at 1/512) where the coverage is at least 1/2.
    constexpr int kSamples = 551; // up to 1.075
    float from = -1;
    for (int i = 0; i <= kSamples; ++i) {
      const float d = kQuadReach * float(i) / kSamples;
      const bool covered = i < kSamples && rimCoverage(d, p.open, p.stat) >= 0.5f;
      if (covered && from < 0)
        from = d;
      else if (!covered && from >= 0) {
        ringMesh(p, kTracedRimOffset, from, d, from <= 0 ? 0.08f : 0.03f, color, vertices, indices);
        from = -1;
      }
    }
    if (indices.empty()) {
      if (m.handle)
        api.DestroyMesh(m.handle);
      m.handle = nullptr;
      m.key = key;
      return nullptr;
    }
  }
  return makeMesh(api, m, key, vertices, indices, material);
}

// The ghost: the engine's quad, a unit in front of the rim.
remixapi_MeshHandle ghostMeshFor(const remixapi_Interface &api, const Portal &p, remixapi_MaterialHandle material) {
  struct {
    float halfWidth, halfHeight;
    int32_t index;
  } keyData = {p.halfWidth, p.halfHeight, p.index};
  const uint64_t key = meshKey(&keyData, sizeof(keyData), 0x67686f7374000000ull);
  Mesh &m = g_ghostMeshes[p.index];
  if (m.handle && m.key == key)
    return m.handle;
  std::vector<remixapi_HardcodedVertex> vertices(4);
  const float w = p.halfWidth, h = p.halfHeight;
  quadVertex(vertices[0], p, kGhostOffset, -w, -h, 0xffffffffu);
  quadVertex(vertices[1], p, kGhostOffset, w, -h, 0xffffffffu);
  quadVertex(vertices[2], p, kGhostOffset, w, h, 0xffffffffu);
  quadVertex(vertices[3], p, kGhostOffset, -w, h, 0xffffffffu);
  return makeMesh(api, m, key, vertices, {0, 1, 2, 0, 2, 3}, material);
}

// The ghost's fade (PortalStaticOverlay's vertex shader, at the portal's
// centre): seen from its front, gone within 120 units and whole from 240;
// from behind, whole.
float ghostFade(const Portal &p, const float *eye) {
  if (!eye)
    return 1;
  float toEye[3], facing = 0, distance2 = 0;
  for (int r = 0; r < 3; ++r) {
    const float centre = p.transform[r][3] + p.transform[r][0] * kGhostOffset;
    toEye[r] = eye[r] - centre;
    facing += p.transform[r][0] * toEye[r];
    distance2 += toEye[r] * toEye[r];
  }
  if (facing < 0)
    return 1;
  return std::clamp((distance2 - 14400.0f) / 43200.0f, 0.0f, 1.0f);
}

} // namespace

bool enabled() {
  static const bool on = x86::envFlag("PORTALS", true);
  return on;
}

bool viewsKept() {
  static const bool on = x86::envFlag("PORTAL_VIEWS", true);
  return on;
}

float solidEnergyLight() {
  static const float v = std::max(0.0f, x86::envFloat("SOLID_ENERGY_LIGHT", 1.0f));
  return v;
}

remixapi_MaterialHandle solidEnergyMaterial(const remixapi_Interface &api, const char *name, float outputIntensity) {
  static const bool on = x86::envFlag("SOLID_ENERGY", true);
  if (!on || !enabled() || !name || _stricmp(name, "effects/projected_wall") != 0 || !std::isfinite(outputIntensity))
    return nullptr;
  // The additive blend (source alpha, one) takes the output's colour at its
  // alpha, both scaled by it: the intensity squared (a 1/256 step).
  const float intensity = std::round(std::clamp(outputIntensity, 0.0f, 16.0f) * 256) / 256;
  uint32_t key;
  std::memcpy(&key, &intensity, sizeof(key));
  if (auto it = g_energyMaterials.find(key); it != g_energyMaterials.end())
    return it->second;
  if (g_energyMaterials.size() >= 16 || !energyTextures(api))
    return nullptr;
  remixapi_MaterialHandle &material = g_energyMaterials[key];
  const auto &t = g_energyTextures;
  wchar_t base[32], detail1[32], detail2[32];
  std::swprintf(base, 32, L"0x%llx", static_cast<unsigned long long>(t.hashes[0]));
  std::swprintf(detail1, 32, L"0x%llx", static_cast<unsigned long long>(t.hashes[1]));
  std::swprintf(detail2, 32, L"0x%llx", static_cast<unsigned long long>(t.hashes[2]));
  // effects/projected_wall.vmt: $detail1 (mod2x) _draw, $detail2 (add) _noise;
  // its proxies: the base scrolls 0.15 a second in v, detail 1 -3 in v and
  // sways in u (Sine 0 to 0.94, period 5), detail 2 0.18 in v (the scales'
  // sub-percent wobbles left out).
  remixapi_MaterialSolidEnergyEXT energy = {};
  energy.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_SOLID_ENERGY_EXT;
  energy.detail1Texture = detail1;
  energy.detail2Texture = detail2;
  energy.intensity = intensity * intensity;
  energy.baseScroll[1] = 0.15f;
  energy.detail1Scroll[1] = -3.0f;
  energy.detail2Scroll[1] = 0.18f;
  energy.detail1Sway[0] = 0.47f;
  energy.detail1Sway[1] = 5.0f;
  remixapi_MaterialInfoOpaqueEXT opaque = {};
  opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
  opaque.pNext = &energy;
  opaque.albedoConstant = {1, 1, 1};
  opaque.opacityConstant = 1;
  opaque.roughnessConstant = 1;
  opaque.alphaTestType = 7; // always
  remixapi_MaterialInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
  info.pNext = &opaque;
  info.hash = 0x656e657267790000ull + key; // "energy"
  info.albedoTexture = base;
  // Its light (solidEnergyLight): the runtime's estimate is the glow averaged
  // over its textures, once per material; the sheet's look does not use this.
  info.emissiveColorConstant = {1, 1, 1};
  info.emissiveIntensity = solidEnergyLight();
  info.filterMode = 1;
  info.wrapModeU = info.wrapModeV = 1;
  if (api.CreateMaterial(&info, &material) != REMIXAPI_ERROR_CODE_SUCCESS || !material) {
    material = nullptr;
    ++g_energyRefused;
    diag::stage("Portal 2: light bridges: the SolidEnergy material (intensity %.3f) was refused", intensity);
  }
  return material;
}

bool isPortalMaterial(const char *name) { return name && _strnicmp(name, "models/portals/", 15) == 0; }

bool ownRims() { return enabled() && rimIntensity() > 0; }

bool engineRims() {
  static const bool on = x86::envFlag("PORTAL_ENGINE_RIMS", false);
  return on;
}

float engineRimDepthScale() {
  static const float v = std::max(1.0f, x86::envFloat("PORTAL_RIM_DEPTH", 1.01f));
  return v;
}

bool install() {
  if (!enabled()) {
    g_status = "off (OPENREMIX_PROXY_PORTALS=0)";
    return false;
  }
  Module client;
  void *list = x86::createInterface("client.dll", "VClientEntityList003");
  void *clientDll = x86::createInterface("client.dll", "VClient016");
  if (!client.open("client.dll") || !list || !clientDll) {
    g_status = "client interfaces not found";
    diag::stage("Portal 2: portals: %s", g_status.load());
    return false;
  }
  void **listSlots = *static_cast<void ***>(list);
  // IBaseClientDLL slot 8: GetAllClasses ("mov eax,[head]; ret"; Portal 2's
  // IBaseClientDLL has no IAppSystem part, L4D2's slot 7 is LevelShutdown
  // here). IClientEntityList slots 3 (GetClientEntity) and 6
  // (GetHighestEntityIndex: "mov eax,[ecx+0x24]; ret").
  void **clientSlots = *static_cast<void ***>(clientDll);
  if (!client.inCode(listSlots[3]) || !client.inCode(listSlots[6]) || !client.inCode(clientSlots[8]) ||
      !x86::bytesAt(client, uintptr_t(clientSlots[8]) - client.base, "A1 ? ? ? ? C3") ||
      !x86::bytesAt(client, uintptr_t(listSlots[6]) - client.base, "8B 41 ? C3")) {
    g_status = "client interface slots not as expected";
    diag::stage("Portal 2: portals: %s", g_status.load());
    return false;
  }
  g_portalVtable = x86::rttiVtable(client, ".?AVC_Prop_Portal@@");
  // ClientClass {create, create event, network name +8, recv table +0x0C, next +0x10}.
  using GetAllClassesFn = const uint8_t *(PROXY_THISCALL *)(void *);
  const uint8_t *table = nullptr;
  unsigned classes = 0;
  for (const uint8_t *c = reinterpret_cast<GetAllClassesFn>(clientSlots[8])(clientDll); c && classes < 4096;
       c = read<const uint8_t *>(c, 0x10), ++classes) {
    const char *name = read<const char *>(c, 8);
    if (name && std::strcmp(name, "CProp_Portal") == 0)
      table = read<const uint8_t *>(c, 0x0C);
  }
  if (!g_portalVtable || !table) {
    g_status = !g_portalVtable ? "C_Prop_Portal vtable not found" : "CProp_Portal client class not found";
    diag::stage("Portal 2: portals: %s (%u client classes)", g_status.load(), classes);
    return false;
  }
  g_vars.origin = x86::findNetProp(table, "m_ptOrigin");
  g_vars.angles = x86::findNetProp(table, "m_qAbsAngle");
  g_vars.linked = x86::findNetProp(table, "m_hLinkedPortal");
  g_vars.isPortal2 = x86::findNetProp(table, "m_bIsPortal2");
  g_vars.activated = x86::findNetProp(table, "m_bActivated");
  g_vars.halfWidth = x86::findNetProp(table, "m_fNetworkHalfWidth");
  g_vars.halfHeight = x86::findNetProp(table, "m_fNetworkHalfHeight");
  g_vars.firedBy = x86::findNetProp(table, "m_hFiredByPlayer");
  g_clientFields = g_vars.origin == kExpected.origin && g_vars.angles == kExpected.angles &&
                   g_vars.linked == kExpected.linked && g_vars.isPortal2 == kExpected.isPortal2 &&
                   g_vars.activated == kExpected.activated && g_vars.halfWidth == kExpected.halfWidth &&
                   g_vars.halfHeight == kExpected.halfHeight;
  diag::stage("Portal 2: portals: C_Prop_Portal vtable client+0x%lX; m_ptOrigin +0x%X m_qAbsAngle +0x%X "
              "m_hLinkedPortal +0x%X m_bIsPortal2 +0x%X m_bActivated +0x%X m_fNetworkHalfWidth +0x%X "
              "m_fNetworkHalfHeight +0x%X m_hFiredByPlayer +0x%X; client-only fields %s",
              (unsigned long)(uintptr_t(g_portalVtable) - client.base), g_vars.origin, g_vars.angles, g_vars.linked,
              g_vars.isPortal2, g_vars.activated, g_vars.halfWidth, g_vars.halfHeight, g_vars.firedBy,
              g_clientFields ? "used" : "NOT used (the layout moved)");
  if (g_vars.origin < 0 || g_vars.angles < 0 || g_vars.linked < 0 || g_vars.isPortal2 < 0 || g_vars.activated < 0) {
    g_status = "CProp_Portal's networked variables not found";
    return false;
  }
  g_entityList = list;
  g_status = g_clientFields ? "ok" : "ok (opening and matrix check off: client-only fields not verified)";
  return true;
}

void sample() {
  if (!g_entityList)
    return;
  using GetEntityFn = const uint8_t *(PROXY_THISCALL *)(void *, int);
  using IntFn = int(PROXY_THISCALL *)(void *);
  void **slots = *static_cast<void ***>(g_entityList);
  const auto getEntity = reinterpret_cast<GetEntityFn>(slots[3]);
  const int highest = std::min(reinterpret_cast<IntFn>(slots[6])(g_entityList), 2047);
  static const bool opening = x86::envFlag("PORTAL_OPENING", true);
  struct Found {
    int entity, linked;
    uint32_t firedBy;
    bool portal2, activated;
    float transform[3][4];
    float halfWidth, halfHeight, open, stat;
    const uint8_t *object;
  };
  std::vector<Found> found;
  for (int i = 1; i <= highest; ++i) {
    const uint8_t *e = getEntity(g_entityList, i);
    if (!e || *reinterpret_cast<void *const *>(e) != g_portalVtable)
      continue;
    Found f = {};
    f.entity = i;
    f.object = e;
    const uint32_t linked = read<uint32_t>(e, g_vars.linked);
    f.linked = linked == 0xFFFFFFFFu ? -1 : int(linked & 0xFFFF); // CBaseHandle: entry index in the low 16 bits
    const uint32_t firedBy = g_vars.firedBy >= 0 ? read<uint32_t>(e, g_vars.firedBy) : 0xFFFFFFFFu;
    f.firedBy = firedBy == 0xFFFFFFFFu ? 0 : (firedBy & 0xFFFF) + 1;
    f.portal2 = read<bool>(e, g_vars.isPortal2);
    f.activated = read<bool>(e, g_vars.activated);
    float origin[3], angles[3];
    std::memcpy(origin, e + g_vars.origin, 12);
    std::memcpy(angles, e + g_vars.angles, 12);
    if (!finite3(origin) || !finite3(angles))
      continue;
    frame(origin, angles, f.transform);
    f.halfWidth = g_vars.halfWidth >= 0 ? read<float>(e, g_vars.halfWidth) : kHalfWidth;
    f.halfHeight = g_vars.halfHeight >= 0 ? read<float>(e, g_vars.halfHeight) : kHalfHeight;
    if (!(f.halfWidth > 1 && f.halfWidth < 1024))
      f.halfWidth = kHalfWidth;
    if (!(f.halfHeight > 1 && f.halfHeight < 1024))
      f.halfHeight = kHalfHeight;
    f.open = 1;
    f.stat = 0;
    if (opening && g_clientFields) {
      const float open = read<float>(e, kOpenAmount);
      if (std::isfinite(open) && open >= 0 && open <= 1)
        f.open = open;
    }
    if (g_clientFields) {
      const float stat = read<float>(e, kStaticAmount);
      if (std::isfinite(stat))
        f.stat = std::clamp(stat, 0.0f, 1.0f);
    }
    found.push_back(f);
  }
  ++g_stats.samples;
  g_stats.found += found.size();
  auto byEntity = [&](int entity) -> const Found * {
    for (const Found &f : found)
      if (f.entity == entity)
        return &f;
    return nullptr;
  };
  for (Slot &s : g_slots)
    s.seen = false;
  std::vector<Portal> portals;
  bool taken[kIndices] = {};
  for (const Found &f : found) {
    if (!f.activated)
      continue;
    ++g_stats.activated;
    const Found *partner = f.linked >= 0 ? byEntity(f.linked) : nullptr;
    if (partner && (!partner->activated || partner->linked != f.entity || partner->portal2 == f.portal2))
      partner = nullptr; // not (yet) a pair
    // The pair's name: its player, else its primary portal's entity.
    uint32_t key = f.firedBy ? 0x80000000u | f.firedBy : 0;
    if (!key) {
      const Found &primary = partner && f.portal2 ? *partner : f;
      key = uint32_t(primary.entity);
    }
    const int slot = slotFor(key);
    if (slot < 0) {
      ++g_stats.overSlots;
      continue;
    }
    Portal p;
    p.entity = f.entity;
    p.index = slot * 2 + (f.portal2 ? 1 : 0);
    if (taken[p.index]) {
      ++g_stats.conflicts;
      continue;
    }
    taken[p.index] = true;
    std::memcpy(p.transform, f.transform, sizeof(p.transform));
    p.halfWidth = f.halfWidth;
    p.halfHeight = f.halfHeight;
    // Quantized (each step a new mesh while it opens or its static fades).
    // The hole as the engine's stencil cuts it; below 5% only the rim shows.
    p.open = std::round(f.open * 32) / 32;
    p.stat = std::round(f.stat * 32) / 32;
    const float k = p.open * p.open * (3 - 2 * p.open);
    p.scale = k * k / 1.075f;
    p.hole = p.scale >= 0.05f;
    if (p.open < 1)
      ++g_stats.opening;
    portals.push_back(p);
    // The engine's own teleport matrix against this one's (linked pairs).
    if (partner && g_clientFields) {
      float mine[3][4];
      thisToLinked(f.transform, partner->transform, mine);
      float engine[4][4];
      std::memcpy(engine, f.object + kThisToLinked, sizeof(engine));
      float rotation = 0, translation = 0;
      for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c)
          rotation = std::max(rotation, std::fabs(mine[r][c] - engine[r][c]));
        translation = std::max(translation, std::fabs(mine[r][3] - engine[r][3]));
      }
      if (std::isfinite(rotation) && std::isfinite(translation)) {
        g_stats.rotationError = std::max(g_stats.rotationError, rotation);
        g_stats.translationError = std::max(g_stats.translationError, translation);
        ++g_stats.compared;
      }
    }
  }
  for (Slot &s : g_slots)
    if (s.used && !s.seen)
      s = {};
  // Partners present among those submitted. A lone portal's rim shows the
  // static whole (the swirl fills it), as the engine's PortalStatic proxy has.
  for (Portal &p : portals) {
    for (const Portal &q : portals)
      p.partner |= q.index == (p.index ^ 1);
    if (!p.partner)
      p.stat = 1;
  }
  for (const Portal &p : portals)
    ++(p.partner ? g_stats.pairs : g_stats.lone);
  g_stats.submitted += portals.size();
  if (g_published && samePortals(g_published->portals, portals))
    return;
  {
    std::string text;
    for (const Portal &p : portals) {
      char item[128];
      std::snprintf(item, sizeof(item), "%sindex %d (entity %d%s, %.0f %.0f %.0f%s, static %.2f)",
                    text.empty() ? "" : ", ", p.index, p.entity, p.partner ? ", paired" : ", lone", p.transform[0][3],
                    p.transform[1][3], p.transform[2][3], p.open < 1 ? ", opening" : "", p.stat);
      text += item;
    }
    std::lock_guard<std::mutex> lock(g_textLock);
    g_lastSet = text.empty() ? "none" : text;
  }
  auto snapshot = std::make_shared<Snapshot>();
  snapshot->portals = std::move(portals);
  g_published = snapshot;
  ++g_stats.published;
  const uint32_t sequence = ++g_sequence;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    g_pending.emplace_back(sequence, std::move(snapshot));
    if (g_pending.size() > 64) // markers not arriving: the newest still win
      g_pending.pop_front();
  }
  announce(sequence);
}

void activate(uint32_t sequence) {
  ++g_stats.received;
  std::lock_guard<std::mutex> lock(g_lock);
  while (!g_pending.empty() && int32_t(sequence - g_pending.front().first) >= 0) {
    g_current = std::move(g_pending.front().second);
    g_pending.pop_front();
  }
}

void submit(const remixapi_Interface &api, const float *eye) {
  std::shared_ptr<const Snapshot> current;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    current = g_current;
  }
  ++g_stats.submits;
  if (!current) {
    ++g_stats.submitsEmpty;
    return;
  }
  const bool effects = (rimIntensity() > 0 || ghosts()) && !current->portals.empty() && effectTextures(api);
  for (const Portal &p : current->portals) {
    if (p.index < 0 || p.index >= kIndices)
      continue;
    remixapi_InstanceIdentityEXT identity = {};
    identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
    identity.classification = REMIXAPI_INSTANCE_DYNAMIC_EXT;
    identity.rasterVisible = 1;
    remixapi_InstanceInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    info.pNext = &identity;
    std::memcpy(info.transform.matrix, p.transform, sizeof(info.transform.matrix));
    info.doubleSided = 1;
    if (p.hole)
      if (const remixapi_MaterialHandle material = materialFor(api, p.index))
        if ((info.mesh = meshFor(api, p, material))) {
          // A stable identity per index (motion vectors, history).
          identity.instanceId = 0x706f7274616c0100ull + uint64_t(p.index);
          api.DrawInstance(&info);
          ++g_stats.drawn;
        }
    if (!effects)
      continue;
    const int color = p.index & 1;
    if (rimIntensity() > 0) {
      // The traced copy: reflection-only (the camera sees the overlay below),
      // its mesh cut to where the rim's coverage is at least 1/2. It lights
      // the scene and shows in reflections and through other portals.
      const remixapi_MaterialHandle tracedMaterial = rimMaterialFor(api, color, true);
      if (tracedMaterial && (info.mesh = rimMeshFor(api, p, tracedMaterial, true))) {
        identity.instanceId = 0x706f7274616c7300ull + uint64_t(p.index);
        identity.rasterVisible = 0;
        info.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_THIRD_PERSON_PLAYER_BODY;
        api.DrawInstance(&info);
        ++g_stats.tracedRims;
      }
      // What the eye sees: the engine's overlay, source alpha over the
      // frame, depth-tested (unless the engine's own rims are replayed).
      const remixapi_MaterialHandle rimMaterial = engineRims() ? nullptr : rimMaterialFor(api, color, false);
      if (rimMaterial && (info.mesh = rimMeshFor(api, p, rimMaterial, false))) {
        remixapi_InstanceRasterStateEXT raster = {};
        raster.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_RASTER_STATE_EXT;
        raster.depthTest = 1;
        raster.depthWrite = 0;
        raster.depthCompare = 3; // LESS_OR_EQUAL
        remixapi_InstanceInfoBlendEXT over = {};
        over.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
        over.pNext = &raster;
        over.alphaBlendEnabled = 1;
        over.srcColorBlendFactor = over.srcAlphaBlendFactor = 6; // SRC_ALPHA
        over.dstColorBlendFactor = over.dstAlphaBlendFactor = 7; // ONE_MINUS_SRC_ALPHA
        identity.pNext = &over;
        identity.instanceId = 0x706f7274616c7200ull + uint64_t(p.index);
        identity.classification = REMIXAPI_INSTANCE_RASTER_ONLY_EXT;
        identity.rasterVisible = 1;
        info.categoryFlags = 0;
        api.DrawInstance(&info);
        identity.pNext = nullptr;
        ++g_stats.rims;
      }
    }
    if (ghosts())
      if (const remixapi_MaterialHandle ghostMaterial = ghostMaterialFor(api, color))
        if ((info.mesh = ghostMeshFor(api, p, ghostMaterial))) {
          const float fade = ghostFade(p, eye);
          if (fade <= 0)
            continue;
          // Premultiplied: colour T.rgb x 0.3 f (the texture factor's colour
          // is display-encoded), alpha T.a x f; drawn where something is
          // nearer than the quad.
          const uint32_t level = uint32_t(std::lround(255 * std::pow(0.3f * fade, 1 / 2.2f)));
          remixapi_InstanceRasterStateEXT raster = {};
          raster.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_RASTER_STATE_EXT;
          raster.depthTest = 1;
          raster.depthWrite = 0;
          raster.depthCompare = 4; // GREATER
          remixapi_InstanceInfoBlendEXT ghost = {};
          ghost.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
          ghost.pNext = &raster;
          ghost.alphaBlendEnabled = 1;
          ghost.srcColorBlendFactor = ghost.srcAlphaBlendFactor = 1; // ONE
          ghost.dstColorBlendFactor = ghost.dstAlphaBlendFactor = 7; // ONE_MINUS_SRC_ALPHA
          ghost.textureColorOperation = ghost.textureAlphaOperation = 3; // modulate
          ghost.textureColorArg1Source = ghost.textureAlphaArg1Source = 1; // texture
          ghost.textureColorArg2Source = ghost.textureAlphaArg2Source = 3; // texture factor
          ghost.tFactor = uint32_t(std::lround(255 * fade)) << 24 | level << 16 | level << 8 | level;
          identity.pNext = &ghost;
          identity.instanceId = 0x706f727467680000ull + uint64_t(p.index);
          identity.classification = REMIXAPI_INSTANCE_RASTER_ONLY_EXT;
          identity.rasterVisible = 1;
          info.categoryFlags = 0;
          api.DrawInstance(&info);
          identity.pNext = nullptr;
          ++g_stats.ghosts;
        }
  }
}

void reset(const remixapi_Interface &api) {
  for (Mesh *meshes : {g_meshes, g_rimMeshes, g_tracedRimMeshes, g_ghostMeshes})
    for (int i = 0; i < kIndices; ++i) {
      if (meshes[i].handle)
        api.DestroyMesh(meshes[i].handle);
      meshes[i] = {};
    }
}

void dumpStatistics(FILE *out) {
  static ULONGLONG last = GetTickCount64();
  const ULONGLONG now = GetTickCount64();
  const double seconds = now > last ? double(now - last) / 1000 : 1;
  last = now;
  const uint64_t samples = g_stats.samples.exchange(0);
  const double perSample = samples ? 1.0 / double(samples) : 0.0;
  fprintf(out, "== Portal 2 portals: %s | views kept %s\n", g_status.load(), viewsKept() ? "yes" : "no");
  fprintf(out,
          "   per sample (%.1f/s): portals found %.2f, activated %.2f, submitted %.2f (paired %.2f, lone %.2f, "
          "opening %.2f) | left out: past %d slots %.2f, index taken %.2f\n",
          double(samples) / seconds, double(g_stats.found.exchange(0)) * perSample,
          double(g_stats.activated.exchange(0)) * perSample, double(g_stats.submitted.exchange(0)) * perSample,
          double(g_stats.pairs.exchange(0)) * perSample, double(g_stats.lone.exchange(0)) * perSample,
          double(g_stats.opening.exchange(0)) * perSample, kSlots, double(g_stats.overSlots.exchange(0)) * perSample,
          double(g_stats.conflicts.exchange(0)) * perSample);
  fprintf(out, "   per second: sets published %.1f, instances drawn %.1f, meshes built %.1f, failed %.1f\n",
          double(g_stats.published.exchange(0)) / seconds, double(g_stats.drawn.exchange(0)) / seconds,
          double(g_stats.meshesBuilt.exchange(0)) / seconds, double(g_stats.failed.exchange(0)) / seconds);
  fprintf(out,
          "   effects per second: rim overlays %.1f, traced rims %.1f, ghosts %.1f | rim scale %.2f%s, ghosts %s | "
          "textures %s\n",
          double(g_stats.rims.exchange(0)) / seconds, double(g_stats.tracedRims.exchange(0)) / seconds,
          double(g_stats.ghosts.exchange(0)) / seconds, rimIntensity(), engineRims() ? " (engine rims replayed)" : "",
          ghosts() ? "on" : "off",
          !g_effectTextures.tried ? "not made yet"
          : g_effectTextures.failure.empty() ? "made"
                                             : g_effectTextures.failure.c_str());
  fprintf(out, "   since launch: sets announced %llu, their markers received %llu | submits %llu, without a set %llu\n",
          (unsigned long long)g_stats.announced.load(), (unsigned long long)g_stats.received.load(),
          (unsigned long long)g_stats.submits.load(), (unsigned long long)g_stats.submitsEmpty.load());
  fprintf(out,
          "   teleport check (m_matrixThisToLinked vs T_B diag(-1,-1,1) T_A^-1): %llu pairs compared, largest "
          "difference rotation %.4f, translation %.3f units\n",
          (unsigned long long)g_stats.compared.load(), g_stats.rotationError, g_stats.translationError);
  // (Render-thread state read without its thread: a stale count is fine.)
  fprintf(out, "   light bridges (remixapi_MaterialSolidEnergyEXT): textures %s, materials %zu, refused %llu\n",
          !g_energyTextures.tried ? "not made yet"
          : g_energyTextures.failure.empty() ? "made"
                                             : g_energyTextures.failure.c_str(),
          g_energyMaterials.size(), (unsigned long long)g_energyRefused.load());
  std::lock_guard<std::mutex> lock(g_textLock);
  fprintf(out, "   indices: %s\n", g_lastSet.c_str());
}

} // namespace proxy::portal2
