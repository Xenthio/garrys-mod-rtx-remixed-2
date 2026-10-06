#include <map>
#include "remix_output.h"
#include "d3d_hooks.h"
#include "remix_ui.h"
#include "sky_faces.h"
#include "bsp_world.h"
#include "dxvk_hash.h"
#include "engine_lights.h"
#include "texture_categories.h"
#include <unordered_set>

#include "texture_formats.h"
#define XXH_INLINE_ALL
#include "xxhash.h"

#define REMIX_ALLOW_X86
#include <remix/remix_c.h>
#include <remix/remix_native_ext.h>
#include <remix/remix_usd_ext.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace proxy::remix {
namespace {

// Source vertex shader registers (common_vs_fxc.h layout).
constexpr unsigned kRegModelViewProj = 4; // c4..c7: model * view * projection (columns)
constexpr unsigned kRegViewProj = 8;  // c8..c11: view * projection
constexpr unsigned kRegModel = 58;    // c58..c60: model (object to world) 3x4 rows
constexpr unsigned kRegAmbientUp = 25; // c21..c26: ambient cube +X -X +Y -Y +Z -Z (linear)
// MaterialMatrixMode_t
constexpr int kMatrixView = 0;
constexpr int kMatrixProjection = 1;
// IShaderAPI (ShaderApi030) slot 12: void GetMatrix(MaterialMatrixMode_t, float *dst)
constexpr unsigned kSlotGetMatrix = 12;
// IShaderAPI slot 34: int GetCurrentNumBones() const
constexpr unsigned kSlotGetCurrentNumBones = 34;
constexpr unsigned kMaxBones = 53; // cModel[53] starting at c58

remixapi_Interface g_api = {};
bool g_ready = false;
HWND g_gameWindow = nullptr;
HWND g_hiddenWindow = nullptr;
void *g_shaderApi = nullptr;
uint64_t g_frame = 0;

struct Stats {
  std::atomic<uint32_t> draws{0}, accepted{0}, offTarget{0}, noPosition{0}, noData{0}, skinned{0}, badPrim{0},
      badRange{0}, cacheHits{0}, created{0}, transient{0}, evicted{0}, submitted{0}, frames{0}, nonFinite{0}, rejected{0}, flat{0}, uiDraws{0}, uiNoTexture{0}, depthOnly{0}, additive{0}, blended{0}, duplicate{0}, skyFrames{0}, lights{0}, playerBody{0}, textures{0}, textureSkipped{0}, ignored{0}, bspDropped{0}, bspKeptBlended{0}, bspKeptDynamic{0}, bspPropsDropped{0}, bspOverlaysDropped{0}, skyViewDemoted{0}, flashlightPasses{0}, worldViewCameras{0};
} g_stats;

// CPU time accounting (microseconds), reset with the statistics.
struct Timer {
  LARGE_INTEGER start;
  std::atomic<uint32_t> &sink; // 32-bit: a 64-bit atomic add is a locked cmpxchg8b loop on x86
  explicit Timer(std::atomic<uint32_t> &s) : sink(s) { QueryPerformanceCounter(&start); }
  ~Timer() {
    static const double usPerTick = [] {
      LARGE_INTEGER f;
      QueryPerformanceFrequency(&f);
      return 1e6 / double(f.QuadPart);
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    sink.fetch_add(uint32_t(double(now.QuadPart - start.QuadPart) * usPerTick), std::memory_order_relaxed);
  }
};
std::atomic<uint64_t> g_transientVerts{0}, g_transientIndices{0};
std::mutex g_transientSampleLock;
std::unordered_map<const void *, uint32_t> g_transientDecls; // declaration -> transient draws
// Microseconds per statistics interval (10 s).
std::atomic<uint32_t> g_usDraw{0}, g_usBuild{0}, g_usSubmit{0}, g_usPresent{0}, g_usFrame{0}, g_usBsp{0};
LARGE_INTEGER g_lastPresent = {};

// Keys for this session's caches and identities (draw state, views, lights).
// (Formerly FNV-1a, byte by byte: a hot spot at ~1200 draws per frame.)
uint64_t fnv(const void *data, size_t size, uint64_t h = 1469598103934665603ull) {
  return XXH3_64bits_withSeed(data, size, h);
}

struct CachedMesh {
  remixapi_MeshHandle handle = nullptr;
  uint64_t lastFrame = 0;
  uint32_t bones = 0; // skinned: bone matrices the mesh indexes (0 = rigid)
  uint64_t geometry = 0; // dxvk::geometryHash of the draw that built it
};
std::unordered_map<uint64_t, CachedMesh> g_meshes;
std::vector<remixapi_MeshHandle> g_transient; // destroyed after Present

// --- Textures and materials ------------------------------------------------
// One Remix texture per uploaded D3D texture (level 0), shared by textures
// that receive it through UpdateTexture. Its material is created on first draw.
struct RemixTexture {
  remixapi_TextureHandle texture = nullptr;
  remixapi_MaterialHandle material = nullptr;
  remixapi_MaterialHandle skyMaterial = nullptr; // tinted by rtx.skyBrightness
  float skyBrightness = 0;
  uint64_t hash = 0;
  ~RemixTexture() {
    if (!g_ready)
      return;
    if (material)
      g_api.DestroyMaterial(material);
    if (skyMaterial)
      g_api.DestroyMaterial(skyMaterial);
    if (texture)
      g_api.DestroyTexture(texture);
  }
};
std::mutex g_textureLock;
std::unordered_map<const void *, std::shared_ptr<RemixTexture>> g_textures;
// D3D textures with identical contents share one record: the dxvk hash names
// the runtime texture and material, and mods key material replacements by it.
std::unordered_map<uint64_t, std::weak_ptr<RemixTexture>> g_texturesByHash;
std::wstring g_moduleDir; // directory of the game executable

// CPU copies of small textures (level 0, tightly packed native format) for UI
// drawing, which needs RGBA8 pixels. Font pages are filled through partial
// updates, which are applied here as well.
struct CpuTexture {
  uint32_t width = 0, height = 0;
  D3DFORMAT format = D3DFMT_UNKNOWN;
  std::vector<uint8_t> data;
  uint32_t version = 0;
  uint32_t registeredVersion = 0; // UI texture contents last registered (0 = never)
  // Texels changed since registration (x0, y0, x1, y1; empty: x1 <= x0):
  // forwarded as a region update (Chromium repaints dirty rectangles).
  uint32_t dirty[4] = {};
  void touch(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
    x1 = std::min(x1, width);
    y1 = std::min(y1, height);
    if (x1 <= x0 || y1 <= y0)
      return;
    if (dirty[2] <= dirty[0] || dirty[3] <= dirty[1]) {
      dirty[0] = x0, dirty[1] = y0, dirty[2] = x1, dirty[3] = y1;
      return;
    }
    dirty[0] = std::min(dirty[0], x0), dirty[1] = std::min(dirty[1], y0);
    dirty[2] = std::max(dirty[2], x1), dirty[3] = std::max(dirty[3], y1);
  }
};
std::unordered_map<const void *, std::shared_ptr<CpuTexture>> g_cpuTextures;

// Bytes per row (or per 4-row block row) and row count of a level.
bool levelLayout(D3DFORMAT format, uint32_t width, uint32_t height, uint32_t &rowBytes, uint32_t &rows) {
  const uint32_t bw = std::max(1u, (width + 3) / 4), bh = std::max(1u, (height + 3) / 4);
  switch (format) {
  case D3DFMT_A8R8G8B8:
  case D3DFMT_X8R8G8B8:
    rowBytes = width * 4;
    rows = height;
    return true;
  case D3DFMT_DXT1:
    rowBytes = bw * 8;
    rows = bh;
    return true;
  case D3DFMT_DXT5:
    rowBytes = bw * 16;
    rows = bh;
    return true;
  default:
    return false;
  }
}

// CPU copies feed UI textures (RegisterUITexture). GMod's Chromium panels
// (main menu, DHTML) are A8R8G8B8 textures of up to 2048x2048 or more.
bool keepCpuCopy(D3DFORMAT format, uint32_t width, uint32_t height) {
  const bool wide = format == D3DFMT_A8R8G8B8 || format == D3DFMT_X8R8G8B8;
  return uint64_t(width) * height <= (wide ? 4096u * 4096u : 512u * 512u);
}

// dxvk-remix variables the proxy implements itself (l4d2-rtx map configs).
std::atomic<float> g_skyBrightness{1.0f};

// The dxvk-remix hash of a D3D texture's contents (0: none, as for dxvk-remix).
uint64_t textureHash(const void *texture) {
  if (!texture)
    return 0;
  std::lock_guard<std::mutex> lock(g_textureLock);
  auto it = g_textures.find(texture);
  return it == g_textures.end() ? 0 : it->second->hash;
}

// Dev aid (OPENREMIX_PROXY_DUMP_KEYS=1): every mesh replacement key drawn is
// appended once to bin/replacement_keys.txt, for matching against mods.
void noteReplacementKey(uint64_t key, char kind = 'M') {
  static const bool dump = [] {
    char v[8] = {};
    return GetEnvironmentVariableA("OPENREMIX_PROXY_DUMP_KEYS", v, sizeof(v)) && v[0] == '1';
  }();
  if (!dump || !key)
    return;
  static std::mutex lock;
  static std::unordered_set<uint64_t> seen;
  std::lock_guard<std::mutex> guard(lock);
  if (!seen.insert(key).second)
    return;
  if (FILE *f = _wfopen((g_moduleDir + L"\\replacement_keys.txt").c_str(), L"a")) {
    fprintf(f, "%c %016llX\n", kind, static_cast<unsigned long long>(key));
    fclose(f);
  }
}

// `skyBrightness` > 0 selects the sky variant, whose tint scales the texture.
// Caller holds g_textureLock.
remixapi_MaterialHandle ensureMaterial(RemixTexture &t, float skyBrightness) {
  const bool sky = skyBrightness > 0;
  if (sky && t.skyMaterial && t.skyBrightness != skyBrightness) {
    g_api.DestroyMaterial(t.skyMaterial);
    t.skyMaterial = nullptr;
  }
  remixapi_MaterialHandle &slot = sky ? t.skyMaterial : t.material;
  if (!slot) {
    wchar_t path[32];
    swprintf(path, 32, L"0x%llX", static_cast<unsigned long long>(t.hash));
    remixapi_MaterialInfoOpaqueEXT opaque = {};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    // The runtime multiplies texture gamma values by the tint (then linearizes),
    // so a linear brightness B is B^(1/2.2).
    const float tint = sky ? std::pow(skyBrightness, 1.0f / 2.2f) : 1.0f;
    opaque.albedoConstant = {tint, tint, tint};
    opaque.opacityConstant = 1;
    opaque.alphaTestType = 7; // always pass (0 would discard everything)
    opaque.useDrawCallAlphaState = 1; // alpha test/blend come from each draw (InstanceInfoBlendEXT)
    remixapi_MaterialInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.pNext = &opaque;
    info.hash = sky ? t.hash ^ 0x5ca1ab1e5ca1ab1eull : t.hash;
    info.albedoTexture = path;
    info.filterMode = 1; // linear (0 is nearest)
    info.wrapModeU = info.wrapModeV = 1;
    if (g_api.CreateMaterial(&info, &slot) != REMIXAPI_ERROR_CODE_SUCCESS)
      slot = nullptr;
    if (sky)
      t.skyBrightness = skyBrightness;
  }
  return slot;
}

remixapi_MaterialHandle materialFor(const void *texture, float skyBrightness = 0) {
  std::lock_guard<std::mutex> lock(g_textureLock);
  auto it = g_textures.find(texture);
  return it == g_textures.end() ? nullptr : ensureMaterial(*it->second, skyBrightness);
}

// The material of an uploaded texture by its dxvk hash, for the BSP world.
bsp::MaterialRef materialForHash(uint64_t hash) {
  std::lock_guard<std::mutex> lock(g_textureLock);
  auto it = g_texturesByHash.find(hash);
  if (it == g_texturesByHash.end())
    return {};
  auto texture = it->second.lock();
  if (!texture)
    return {};
  bsp::MaterialRef ref;
  ref.handle = ensureMaterial(*texture, 0);
  ref.keep = std::move(texture);
  return ref;
}

// Depth of IVRenderView::DrawWorldLists calls (openremix_proxy_world_lists).
int g_worldLists = 0;
// Dynamic world-list draws kept for the engine, by base texture (statistics).
std::unordered_map<uint64_t, uint32_t> g_keptWorldTextures;
// Depth of COverlayMgr::RenderOverlays calls (openremix_proxy_overlays).
int g_overlays = 0;
// Categories l4d2-rtx applies to the draws that follow (onMarker "body").
uint32_t g_markerCategories = 0;

// --- Lights ------------------------------------------------------------------
// LightDesc_t (public/mathlib/lightdesc.h), 88 bytes.
struct SourceLight {
  int32_t type; // 1 point, 2 directional, 3 spot
  float color[3], position[3], direction[3];
  float range, falloff, attenuation0, attenuation1, attenuation2, theta, phi, thetaDot, phiDot;
};
constexpr size_t kLightDescSize = 88;
std::mutex g_lightLock;
std::unordered_map<uint64_t, SourceLight> g_frameLights, g_lastLights;
struct CachedLight {
  remixapi_LightHandle handle = nullptr;
  uint64_t lastFrame = 0;
  uint64_t definition = 0; // see g_lightDefinitions
};
std::unordered_map<uint64_t, CachedLight> g_lights;
// This frame's lights whose hash is an identity, not their contents (styled
// map lights, dlights, projected textures): the contents' hash. When it
// changes the Remix light is redefined under the same hash.
std::unordered_map<uint64_t, uint64_t> g_lightDefinitions;

float envScale(const char *name, float fallback) {
  char v[32] = {};
  const float value = GetEnvironmentVariableA(name, v, sizeof(v)) ? float(atof(v)) : 0;
  return value > 0 ? value : fallback;
}

// Source colors are linear intensities with 1/(a0 + a1 d + a2 d^2) falloff in
// game units. A point light becomes a small sphere of the same radiant
// intensity (matched at a reference distance when the falloff is not purely
// quadratic). The runtime shades the distant light as brdf * cos * radiance,
// i.e. its "radiance" is the irradiance, which is what Source's color is.
remixapi_LightHandle createLight(const SourceLight &l, uint64_t hash) {
  static const float lightScale = envScale("OPENREMIX_PROXY_LIGHT_SCALE", 1.0f);
  static const float sunScale = envScale("OPENREMIX_PROXY_SUN_SCALE", 1.0f);
  remixapi_LightInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  info.hash = hash;
  remixapi_LightInfoSphereEXT sphere = {};
  remixapi_LightInfoDistantEXT distant = {};
  if (l.type == 2) {
    constexpr float kSunDiameterDegrees = 0.5f;
    info.radiance = {l.color[0] * sunScale, l.color[1] * sunScale, l.color[2] * sunScale};
    const float len = std::sqrt(l.direction[0] * l.direction[0] + l.direction[1] * l.direction[1] +
                                l.direction[2] * l.direction[2]);
    if (len < 1e-6f)
      return nullptr;
    distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
    distant.direction = {l.direction[0] / len, l.direction[1] / len, l.direction[2] / len};
    distant.angularDiameterDegrees = kSunDiameterDegrees;
    info.pNext = &distant;
  } else {
    constexpr float kRadius = 2.0f, kReference = 100.0f;
    const float falloff = l.attenuation0 + l.attenuation1 * kReference + l.attenuation2 * kReference * kReference;
    if (falloff <= 0)
      return nullptr;
    const float intensity = kReference * kReference / falloff; // radiant intensity per unit color
    const float s = lightScale * intensity / (3.14159265f * kRadius * kRadius);
    info.radiance = {l.color[0] * s, l.color[1] * s, l.color[2] * s};
    sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    sphere.position = {l.position[0], l.position[1], l.position[2]};
    sphere.radius = kRadius;
    if (l.type == 3) {
      const float len = std::sqrt(l.direction[0] * l.direction[0] + l.direction[1] * l.direction[1] +
                                  l.direction[2] * l.direction[2]);
      if (len > 1e-6f && l.phiDot > -1 && l.phiDot < 1) {
        sphere.shaping_hasvalue = 1;
        sphere.shaping_value.direction = {l.direction[0] / len, l.direction[1] / len, l.direction[2] / len};
        const float outer = std::acos(l.phiDot), inner = std::acos(std::clamp(l.thetaDot, -1.0f, 1.0f));
        sphere.shaping_value.coneAngleDegrees = outer * 57.2957795f;
        sphere.shaping_value.coneSoftness = outer > 0 ? std::clamp((outer - inner) / outer, 0.0f, 1.0f) : 0;
        sphere.shaping_value.focusExponent = 0;
      }
    }
    info.pNext = &sphere;
  }
  for (float c : {info.radiance.x, info.radiance.y, info.radiance.z})
    if (!std::isfinite(c) || c < 0)
      return nullptr;
  remixapi_LightHandle handle = nullptr;
  return g_api.CreateLight(&info, &handle) == REMIXAPI_ERROR_CODE_SUCCESS ? handle : nullptr;
}

} // namespace
// This frame's 2D draws (HUD, VGUI), submitted as one UI draw list.
std::vector<remixapi_UIVertex> g_uiVertices;
std::vector<uint32_t> g_uiIndices;
std::vector<remixapi_UIDrawCommand> g_uiCommands;
uint32_t g_displayWidth = 0, g_displayHeight = 0;

// In-process UI contributors (l4d2-rtx's ImGui menu): called while the frame's
// UI list is being built, they add draws with openremix_ui_append.
using UiCallback = void(__cdecl *)();
UiCallback g_uiCallback = nullptr;
namespace {

struct Instance {
  remixapi_MeshHandle mesh;
  remixapi_MaterialHandle material;
  const void *texture0; // sky variant material
  remixapi_InstanceInfoBlendEXT blend;
  uint32_t categories;
  uint64_t replacementHash; // dxvk-remix mesh replacement key (0: none)
  bool persistent;          // mesh from static buffers (cached across frames)
  bool singleSided = false; // blended draw D3D culls: its back faces are not seen
  std::vector<remixapi_Transform> bones; // skinned instances
  float transform[3][4];
  uint64_t view;
};
std::vector<Instance> g_instances;

struct View {
  std::vector<std::array<float, 3>> ambientUp; // +Z ambient cube of lit models in this view
  uint32_t draws = 0;
  uint32_t order = 0; // first use within the frame
  bool perspective = false;
  float view[16], projection[16];
};
uint32_t g_viewOrder = 0;
// The views the engine drew a world for this frame (the matrices in effect at
// each outermost "world_lists 1" marker, in order): the main view and, before
// it, the 3D skybox view. The view model and overlays never draw a world.
struct WorldView {
  float view[16], projection[16];
};
std::vector<WorldView> g_worldViews;

// D3DBLEND / D3DBLENDOP / D3DCMP to the Vulkan enums InstanceInfoBlendEXT uses.
uint32_t vkBlendFactor(uint32_t d3d) {
  switch (d3d) {
  case D3DBLEND_ZERO: return 0;
  case D3DBLEND_ONE: return 1;
  case D3DBLEND_SRCCOLOR: return 2;
  case D3DBLEND_INVSRCCOLOR: return 3;
  case D3DBLEND_DESTCOLOR: return 4;
  case D3DBLEND_INVDESTCOLOR: return 5;
  case D3DBLEND_SRCALPHA: return 6;
  case D3DBLEND_INVSRCALPHA: return 7;
  case D3DBLEND_DESTALPHA: return 8;
  case D3DBLEND_INVDESTALPHA: return 9;
  case D3DBLEND_BLENDFACTOR: return 10;
  case D3DBLEND_INVBLENDFACTOR: return 11;
  case D3DBLEND_SRCALPHASAT: return 14;
  default: return 1;
  }
}
uint32_t vkBlendOp(uint32_t d3d) { return d3d >= D3DBLENDOP_ADD && d3d <= D3DBLENDOP_MAX ? d3d - 1 : 0; }
uint32_t vkCompareOp(uint32_t d3d) { return d3d >= D3DCMP_NEVER && d3d <= D3DCMP_ALWAYS ? d3d - 1 : 7; }

remixapi_InstanceInfoBlendEXT blendState(const Draw &d) {
  remixapi_InstanceInfoBlendEXT b = {};
  b.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
  b.alphaTestEnabled = d.alphaTest;
  b.alphaTestReferenceValue = uint8_t(d.alphaRef);
  b.alphaTestCompareOp = d.alphaTest ? vkCompareOp(d.alphaFunc) : 7;
  b.alphaBlendEnabled = d.blend;
  if (d.blend) {
    b.srcColorBlendFactor = vkBlendFactor(d.srcBlend);
    b.dstColorBlendFactor = vkBlendFactor(d.dstBlend);
    b.colorBlendOp = vkBlendOp(d.blendOp);
    b.srcAlphaBlendFactor = vkBlendFactor(d.separateAlpha ? d.srcBlendAlpha : d.srcBlend);
    b.dstAlphaBlendFactor = vkBlendFactor(d.separateAlpha ? d.dstBlendAlpha : d.dstBlend);
    b.alphaBlendOp = vkBlendOp(d.separateAlpha ? d.blendOpAlpha : d.blendOp);
    b.writeMask = d.colorWrite & 15;
  }
  return b;
}

// dxvk-remix fork InstanceCategories (bit order of l4d2-rtx's enum) to
// remixapi_InstanceCategoryBit; the orders diverge from bit 8.
uint32_t remixCategories(uint32_t fork) {
  static const int8_t map[25] = {0,  1,  2,  3,  4,  5,  6,  7,  21, 8,  9,  10, 11,
                                 12, 13, 14, 15, 16, 17, 18, 19, 20, 22, 23, -1};
  uint32_t out = 0;
  for (int bit = 0; bit < 25; ++bit)
    if ((fork >> bit & 1) && map[bit] >= 0)
      out |= 1u << map[bit];
  return out;
}

// World-space transform that makes geometry seen through `main` project like
// it does through a camera at the same place with a narrower field of view:
// view-space x and y scale by the projections' ratios (world' = V^-1 S V world).
// View matrices are row-vector (p_view = p * V) and rigid.
void viewScaleTransform(const float *view, float sx, float sy, float out[3][4]) {
  float r[3][3], t[3];
  for (int j = 0; j < 3; ++j) {
    for (int i = 0; i < 3; ++i)
      r[j][i] = view[i * 4 + j]; // column-vector rotation: v = r p + t
    t[j] = view[12 + j];
  }
  const float s[3] = {sx, sy, 1.0f};
  for (int a = 0; a < 3; ++a) {
    for (int b = 0; b < 3; ++b) {
      float sum = 0;
      for (int k = 0; k < 3; ++k)
        sum += r[k][a] * s[k] * r[k][b]; // (r^T S r)[a][b]
      out[a][b] = sum;
    }
    float sum = 0;
    for (int k = 0; k < 3; ++k)
      sum += r[k][a] * (s[k] - 1.0f) * t[k]; // r^T (S - I) t
    out[a][3] = sum;
  }
}

// a = t * a for 3x4 affine (column-vector) transforms.
void premultiply(const float t[3][4], float a[3][4]) {
  float out[3][4];
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 4; ++c) {
      float sum = c == 3 ? t[r][3] : 0.0f;
      for (int k = 0; k < 3; ++k)
        sum += t[r][k] * a[k][c];
      out[r][c] = sum;
    }
  }
  std::memcpy(a, out, sizeof(out));
}

bool sameRotation(const float *a, const float *b) {
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c)
      if (std::fabs(a[r * 4 + c] - b[r * 4 + c]) > 1e-4f)
        return false;
  return true;
}

// Same rotation and translation: the same camera (e.g. another near/far plane).
bool samePlacement(const float *a, const float *b) {
  if (!sameRotation(a, b))
    return false;
  for (int i = 12; i < 15; ++i)
    if (std::fabs(a[i] - b[i]) > 1e-2f)
      return false;
  return true;
}
std::unordered_map<uint64_t, View> g_views;
View g_lastCamera; // for statistics
float g_lastSky[3] = {};
std::string g_viewSummary; // last frame's views, for statistics

int currentNumBones() {
  using Fn = int(__thiscall *)(void *);
  auto *vtbl = *static_cast<void ***>(g_shaderApi);
  return reinterpret_cast<Fn>(vtbl[kSlotGetCurrentNumBones])(g_shaderApi);
}

void getMatrix(int mode, float *out) {
  using Fn = void(__thiscall *)(void *, int, float *);
  auto *vtbl = *static_cast<void ***>(g_shaderApi);
  reinterpret_cast<Fn>(vtbl[kSlotGetMatrix])(g_shaderApi, mode, out);
}

// Source's packed normal (UBYTE4, bytes 0-1; bytes 2-3 hold the tangent),
// ported from UnpackNormal_UBYTE4 in public/mathlib/mathlib.h.
void unpackNormalUByte4(const uint8_t *packed, float out[3]) {
  float x = packed[0] - 128.0f, y = packed[1] - 128.0f;
  const float zSignBit = x < 0 ? 1.0f : 0.0f, tSignBit = y < 0 ? 1.0f : 0.0f;
  const float zSign = -(2 * zSignBit - 1), tSign = -(2 * tSignBit - 1);
  x = x * zSign - zSignBit - 64;
  y = y * tSign - tSignBit - 64;
  const float xSignBit = x < 0 ? 1.0f : 0.0f, ySignBit = y < 0 ? 1.0f : 0.0f;
  const float xSign = -(2 * xSignBit - 1), ySign = -(2 * ySignBit - 1);
  x = (x * xSign - xSignBit) / 63.0f;
  y = (y * ySign - ySignBit) / 63.0f;
  const float z = 1.0f - x - y;
  const float oolen = 1.0f / std::sqrt(x * x + y * y + z * z);
  out[0] = x * oolen * xSign;
  out[1] = y * oolen * ySign;
  out[2] = z * oolen * zSign;
}

const D3DVERTEXELEMENT9 *findElement(const D3DVERTEXELEMENT9 *e, BYTE usage, BYTE index) {
  for (; e->Stream != 0xff; ++e)
    if (e->Usage == usage && e->UsageIndex == index)
      return e;
  return nullptr;
}

// UI texture (re)registrations: count, bytes and time since the last dump.
std::atomic<uint32_t> g_usUiRegister{0}, g_usUiRegisterApi{0};
std::atomic<uint64_t> g_uiRegistrations{0}, g_uiRegionUpdates{0}, g_uiRegisteredBytes{0};

// Registers (or refreshes) the UI texture for a D3D texture; 0 when the
// texture has no CPU copy (e.g. render targets).
remixapi_UITextureHandle uiTextureFor(const void *texture) {
  std::lock_guard<std::mutex> lock(g_textureLock);
  auto it = g_cpuTextures.find(texture);
  if (it == g_cpuTextures.end())
    return 0;
  auto &t = *it->second;
  const auto id = static_cast<remixapi_UITextureHandle>(reinterpret_cast<uintptr_t>(it->second.get()));
  // A registered 32-bit texture changes in place: only its dirty region goes
  // to the runtime (remixapi_UpdateUITextureRegionEXT), without a copy when
  // its alpha is real.
  static const auto updateRegion = reinterpret_cast<PFN_remixapi_UpdateUITextureRegionEXT>(
      GetProcAddress(GetModuleHandleW(L"remix.dll"), "remixapi_UpdateUITextureRegionEXT"));
  const bool wide = t.format == D3DFMT_A8R8G8B8 || t.format == D3DFMT_X8R8G8B8;
  if (t.registeredVersion && t.registeredVersion != t.version && updateRegion && wide &&
      t.data.size() == size_t(t.width) * t.height * 4) {
    const uint32_t x0 = t.dirty[0], y0 = t.dirty[1], x1 = t.dirty[2], y1 = t.dirty[3];
    remixapi_ErrorCode updated = REMIXAPI_ERROR_CODE_SUCCESS;
    if (x1 > x0 && y1 > y0) {
      Timer timer(g_usUiRegister);
      ++g_uiRegionUpdates;
      g_uiRegisteredBytes += uint64_t(x1 - x0) * (y1 - y0) * 4;
      const uint8_t *rows = t.data.data() + (size_t(y0) * t.width + x0) * 4;
      uint32_t pitch = t.width * 4;
      std::vector<uint8_t> opaque;
      if (t.format == D3DFMT_X8R8G8B8) {
        opaque.resize(size_t(x1 - x0) * (y1 - y0) * 4);
        for (uint32_t y = 0; y < y1 - y0; ++y)
          std::memcpy(opaque.data() + size_t(y) * (x1 - x0) * 4, rows + size_t(y) * pitch, size_t(x1 - x0) * 4);
        for (size_t i = 3; i < opaque.size(); i += 4)
          opaque[i] = 0xff;
        rows = opaque.data();
        pitch = (x1 - x0) * 4;
      }
      Timer apiTimer(g_usUiRegisterApi);
      updated = updateRegion(id, x0, y0, x1 - x0, y1 - y0, REMIXAPI_FORMAT_B8G8R8A8_UNORM, rows, pitch);
    }
    if (updated == REMIXAPI_ERROR_CODE_SUCCESS) {
      std::fill_n(t.dirty, 4, 0u);
      t.registeredVersion = t.version;
      return id;
    }
  }
  if (t.registeredVersion != t.version) {
    Timer timer(g_usUiRegister);
    ++g_uiRegistrations;
    g_uiRegisteredBytes += uint64_t(t.width) * t.height * 4;
    std::fill_n(t.dirty, 4, 0u);
    std::vector<uint8_t> rgba;
    remixapi_Format format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
    if (t.format == D3DFMT_DXT1) {
      rgba = formats::decodeBC1(t.data.data(), t.width, t.height);
    } else if (t.format == D3DFMT_DXT5) {
      rgba = formats::decodeBC3(t.data.data(), t.width, t.height);
    } else {
      rgba = t.data; // BGRA in memory
      format = REMIXAPI_FORMAT_B8G8R8A8_UNORM;
      if (t.format == D3DFMT_X8R8G8B8)
        for (size_t i = 3; i < rgba.size(); i += 4)
          rgba[i] = 0xff;
    }
    remixapi_ErrorCode registered;
    {
      Timer apiTimer(g_usUiRegisterApi);
      registered = g_api.RegisterUITexture(id, t.width, t.height, format, rgba.data(), rgba.size());
    }
    if (registered != REMIXAPI_ERROR_CODE_SUCCESS)
      return 0;
    t.registeredVersion = t.version;
  }
  return id;
}

// Appends a 2D draw to this frame's UI draw list: positions go through the
// draw's model-view-projection registers to screen pixels.
void captureUi(const Draw &d) {
  const auto *pos = findElement(d.elements, D3DDECLUSAGE_POSITION, 0);
  const auto *tex = findElement(d.elements, D3DDECLUSAGE_TEXCOORD, 0);
  const auto *col = findElement(d.elements, D3DDECLUSAGE_COLOR, 0);
  if (!pos || !d.indices.data || !g_displayWidth || !g_displayHeight)
    return;
  remixapi_UITextureHandle texture = 0;
  if (d.texture0 && !(texture = uiTextureFor(d.texture0))) {
    ++g_stats.uiNoTexture; // render-target sampling (post-processing): not UI
    return;
  }
  // Indices first (the declared vertex range may cover a whole buffer).
  std::vector<uint32_t> indices;
  auto index = [&](uint32_t i, uint32_t &out) {
    const uint64_t at = uint64_t(d.startIndex + i) * (d.indices.is32 ? 4 : 2);
    if (at + (d.indices.is32 ? 4 : 2) > d.indices.size)
      return false;
    uint32_t value = 0;
    std::memcpy(&value, d.indices.data + at, d.indices.is32 ? 4 : 2);
    out = value;
    return value >= d.minIndex;
  };
  if (d.type == D3DPT_TRIANGLELIST) {
    indices.resize(size_t(d.primCount) * 3);
    for (uint32_t i = 0; i < indices.size(); ++i)
      if (!index(i, indices[i]))
        return;
  } else if (d.type == D3DPT_TRIANGLESTRIP) {
    for (uint32_t i = 0; i < d.primCount; ++i) {
      uint32_t a, b, c;
      if (!index(i, a) || !index(i + 1, b) || !index(i + 2, c))
        return;
      if (a == b || b == c || a == c)
        continue;
      if (i & 1)
        std::swap(a, b);
      indices.insert(indices.end(), {a, b, c});
    }
  } else {
    return;
  }
  if (indices.empty())
    return;
  const auto [lo, hi] = std::minmax_element(indices.begin(), indices.end());
  const uint32_t low = *lo, count = *hi - *lo + 1;

  auto fetch = [&](const D3DVERTEXELEMENT9 *e, uint32_t v, uint32_t bytes) -> const uint8_t * {
    const auto &s = d.streams[e->Stream];
    if (!s.data)
      return nullptr;
    const uint64_t at = uint64_t(s.offset) + uint64_t(int64_t(d.baseVertex) + v) * s.stride + e->Offset;
    return at + bytes <= s.size ? s.data + at : nullptr;
  };
  const float *mvp = d.vsConstants + kRegModelViewProj * 4; // register k = column k
  const uint32_t base = uint32_t(g_uiVertices.size());
  for (uint32_t v = low; v < low + count; ++v) {
    remixapi_UIVertex out = {};
    float p[3] = {};
    if (const uint8_t *src = fetch(pos, v, 12))
      std::memcpy(p, src, 12);
    float clip[4];
    for (int k = 0; k < 4; ++k)
      clip[k] = p[0] * mvp[k * 4] + p[1] * mvp[k * 4 + 1] + p[2] * mvp[k * 4 + 2] + mvp[k * 4 + 3];
    const float w = std::fabs(clip[3]) > 1e-6f ? clip[3] : 1.0f;
    out.x = (clip[0] / w * 0.5f + 0.5f) * g_displayWidth;
    out.y = (0.5f - clip[1] / w * 0.5f) * g_displayHeight;
    if (tex && tex->Type >= D3DDECLTYPE_FLOAT2 && tex->Type <= D3DDECLTYPE_FLOAT4)
      if (const uint8_t *t = fetch(tex, v, 8)) {
        std::memcpy(&out.u, t, 4);
        std::memcpy(&out.v, t + 4, 4);
      }
    out.color = 0xffffffff;
    if (col && col->Type == D3DDECLTYPE_D3DCOLOR)
      if (const uint8_t *c = fetch(col, v, 4)) // memory B, G, R, A -> R in the low byte
        out.color = uint32_t(c[2]) | uint32_t(c[1]) << 8 | uint32_t(c[0]) << 16 | uint32_t(c[3]) << 24;
    g_uiVertices.push_back(out);
  }
  remixapi_UIDrawCommand command = {};
  command.textureId = texture;
  command.indexCount = uint32_t(indices.size());
  command.indexOffset = uint32_t(g_uiIndices.size());
  for (uint32_t i : indices)
    g_uiIndices.push_back(base + i - low);
  g_uiCommands.push_back(command);
  ++g_stats.uiDraws;
}

// Converts one indexed draw into a Remix mesh. Returns nullptr when the draw
// cannot be represented yet.
// `normalOffset` > 0 lifts the surface off its receiver (decals, see onDraw).
remixapi_MeshHandle buildMesh(const Draw &d, uint64_t hash, uint32_t bonesPerVertex, uint32_t *boneCount,
                              float normalOffset = 0) {
  Timer timer(g_usBuild);
  const auto *pos = findElement(d.elements, D3DDECLUSAGE_POSITION, 0);
  const auto *boneWeights = findElement(d.elements, D3DDECLUSAGE_BLENDWEIGHT, 0);
  const auto *boneIndices = findElement(d.elements, D3DDECLUSAGE_BLENDINDICES, 0);
  const auto *tex = findElement(d.elements, D3DDECLUSAGE_TEXCOORD, 0);
  const auto *nrm = findElement(d.elements, D3DDECLUSAGE_NORMAL, 0);
  const auto &ps = d.streams[pos->Stream];

  std::vector<uint32_t> indices;
  auto index = [&](uint32_t i, uint32_t &out) {
    const uint64_t at = uint64_t(d.startIndex + i) * (d.indices.is32 ? 4 : 2);
    if (at + (d.indices.is32 ? 4 : 2) > d.indices.size)
      return false;
    uint32_t value = 0;
    std::memcpy(&value, d.indices.data + at, d.indices.is32 ? 4 : 2);
    if (value < d.minIndex || value - d.minIndex >= d.numVertices)
      return false;
    out = value - d.minIndex;
    return true;
  };
  if (d.type == D3DPT_TRIANGLELIST) {
    indices.resize(size_t(d.primCount) * 3);
    for (uint32_t i = 0; i < indices.size(); ++i)
      if (!index(i, indices[i])) {
        ++g_stats.badRange;
        return nullptr;
      }
  } else if (d.type == D3DPT_TRIANGLESTRIP) {
    indices.reserve(size_t(d.primCount) * 3);
    for (uint32_t i = 0; i < d.primCount; ++i) {
      uint32_t a, b, c;
      if (!index(i, a) || !index(i + 1, b) || !index(i + 2, c)) {
        ++g_stats.badRange;
        return nullptr;
      }
      if (a == b || b == c || a == c)
        continue;
      if (i & 1)
        std::swap(a, b);
      indices.insert(indices.end(), {a, b, c});
    }
    if (indices.empty())
      return nullptr;
  } else {
    ++g_stats.badPrim;
    return nullptr;
  }

  // Dynamic draws declare the whole buffer as their vertex range; convert only
  // the span the indices reference and rebase the indices onto it.
  const auto [lo, hi] = std::minmax_element(indices.begin(), indices.end());
  const uint32_t low = *lo, count = *hi - *lo + 1;
  for (auto &i : indices)
    i -= low;
  const uint32_t first = uint32_t(d.baseVertex) + d.minIndex + low;
  if (d.streams[pos->Stream].dynamic)
    g_transientVerts += count;

  std::vector<remixapi_HardcodedVertex> vertices(count);
  auto fetch = [&](const D3DVERTEXELEMENT9 *e, uint32_t v, uint32_t bytes) -> const uint8_t * {
    const auto &s = d.streams[e->Stream];
    if (!s.data)
      return nullptr;
    const uint64_t at = uint64_t(s.offset) + uint64_t(first + v) * s.stride + e->Offset;
    return at + bytes <= s.size ? s.data + at : nullptr;
  };
  for (uint32_t v = 0; v < count; ++v) {
    auto &out = vertices[v];
    const uint8_t *p = fetch(pos, v, 12);
    if (!p) {
      ++g_stats.badRange;
      return nullptr;
    }
    std::memcpy(out.position, p, 12);
    if (!std::isfinite(out.position[0]) || !std::isfinite(out.position[1]) || !std::isfinite(out.position[2])) {
      ++g_stats.nonFinite; // the runtime rejects the whole mesh
      return nullptr;
    }
    if (tex && (tex->Type == D3DDECLTYPE_FLOAT2 || tex->Type == D3DDECLTYPE_FLOAT3 ||
                tex->Type == D3DDECLTYPE_FLOAT4)) {
      if (const uint8_t *t = fetch(tex, v, 8))
        std::memcpy(out.texcoord, t, 8);
      if (!std::isfinite(out.texcoord[0]) || !std::isfinite(out.texcoord[1]))
        out.texcoord[0] = out.texcoord[1] = 0;
    }
    if (nrm && nrm->Type == D3DDECLTYPE_UBYTE4) {
      if (const uint8_t *n = fetch(nrm, v, 4))
        unpackNormalUByte4(n, out.normal);
    } else if (nrm && nrm->Type == D3DDECLTYPE_FLOAT3) {
      if (const uint8_t *n = fetch(nrm, v, 12))
        std::memcpy(out.normal, n, 12);
      if (!std::isfinite(out.normal[0]) || !std::isfinite(out.normal[1]) || !std::isfinite(out.normal[2]))
        out.normal[0] = out.normal[1] = out.normal[2] = 0;
    }
    out.color = 0xffffffff;
  }
  (void)ps;

  // Source skinning (common_vs_fxc.h): up to three bone indices in a
  // D3DCOLOR (memory bytes 0..2), two stored weights (float2, or short2 / 32767)
  // with the last weight implied; the shader's bone count selects how many apply.
  std::vector<float> weights;
  std::vector<uint32_t> bones;
  uint32_t maxBone = 0;
  if (bonesPerVertex) {
    weights.resize(size_t(count) * bonesPerVertex);
    bones.resize(weights.size());
    for (uint32_t v = 0; v < count; ++v) {
      const uint8_t *bi = fetch(boneIndices, v, 4);
      float stored[2] = {1, 0};
      if (boneWeights && boneWeights->Type == D3DDECLTYPE_FLOAT2) {
        if (const uint8_t *bw = fetch(boneWeights, v, 8))
          std::memcpy(stored, bw, 8);
      } else if (boneWeights && boneWeights->Type == D3DDECLTYPE_SHORT2) {
        if (const uint8_t *bw = fetch(boneWeights, v, 4)) {
          int16_t packed[2];
          std::memcpy(packed, bw, 4);
          stored[0] = packed[0] / 32767.0f;
          stored[1] = packed[1] / 32767.0f;
        }
      }
      float w[3] = {1, 0, 0};
      if (bonesPerVertex == 2) {
        w[0] = stored[0];
        w[1] = 1 - stored[0];
      } else if (bonesPerVertex == 3) {
        w[0] = stored[0];
        w[1] = stored[1];
        w[2] = 1 - stored[0] - stored[1];
      }
      for (uint32_t k = 0; k < bonesPerVertex; ++k) {
        const uint32_t bone = bi ? bi[k] : 0;
        // The implied weight can round slightly below zero; the runtime rejects negatives.
        weights[size_t(v) * bonesPerVertex + k] = std::isfinite(w[k]) ? std::clamp(w[k], 0.0f, 1.0f) : 0;
        bones[size_t(v) * bonesPerVertex + k] = bone;
        maxBone = std::max(maxBone, bone);
      }
    }
    if (maxBone >= kMaxBones) {
      ++g_stats.badRange;
      return nullptr;
    }
  }

  if (normalOffset > 0) {
    // Vertex normal where present, else the summed face normals.
    std::vector<float> faces(vertices.size() * 3, 0.0f);
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
      const float *a = vertices[indices[i]].position, *b = vertices[indices[i + 1]].position,
                  *c = vertices[indices[i + 2]].position;
      const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
      const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
      for (int k = 0; k < 3; ++k)
        for (int j = 0; j < 3; ++j)
          faces[size_t(indices[i + k]) * 3 + j] += n[j];
    }
    for (size_t v = 0; v < vertices.size(); ++v) {
      const float *vn = vertices[v].normal, *fn = &faces[v * 3];
      const float vlen = std::sqrt(vn[0] * vn[0] + vn[1] * vn[1] + vn[2] * vn[2]);
      const float flen = std::sqrt(fn[0] * fn[0] + fn[1] * fn[1] + fn[2] * fn[2]);
      const float *n = vlen > 1e-6f ? vn : fn;
      const float len = vlen > 1e-6f ? vlen : flen;
      if (len > 1e-12f)
        for (int j = 0; j < 3; ++j)
          vertices[v].position[j] += n[j] / len * normalOffset;
    }
  }

  remixapi_MeshInfoSurfaceTriangles surface = {};
  surface.vertices_values = vertices.data();
  surface.vertices_count = vertices.size();
  surface.indices_values = indices.data();
  surface.indices_count = indices.size();
  if (bonesPerVertex) {
    surface.skinning_hasvalue = 1;
    surface.skinning_value.bonesPerVertex = bonesPerVertex;
    surface.skinning_value.blendWeights_values = weights.data();
    surface.skinning_value.blendWeights_count = uint32_t(weights.size());
    surface.skinning_value.blendIndices_values = bones.data();
    surface.skinning_value.blendIndices_count = uint32_t(bones.size());
  }
  if (boneCount)
    *boneCount = bonesPerVertex ? maxBone + 1 : 0;
  remixapi_MeshInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
  info.hash = hash ? hash : 1;
  info.surfaces_values = &surface;
  info.surfaces_count = 1;
  remixapi_MeshHandle handle = nullptr;
  if (g_api.CreateMesh(&info, &handle) != REMIXAPI_ERROR_CODE_SUCCESS) {
    ++g_stats.rejected;
    return nullptr;
  }
  ++g_stats.created;
  return handle;
}

LRESULT CALLBACK hiddenProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

remixapi_ErrorCode g_modsResult = REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;

// Dev aid: `bin/remix_screenshot.request` (its contents: an absolute PNG path,
// or empty for bin/remix_screenshot.png) asks for a screenshot of the next
// presented frame.
void serviceScreenshotRequest() {
  // Checked every 16th frame: a file system call per frame shows in profiles.
  if (g_frame % 16)
    return;
  const std::wstring request = g_moduleDir + L"\\remix_screenshot.request";
  if (GetFileAttributesW(request.c_str()) == INVALID_FILE_ATTRIBUTES)
    return;
  char path[MAX_PATH] = {};
  if (FILE *f = _wfopen(request.c_str(), L"r")) {
    if (!fgets(path, sizeof(path), f))
      path[0] = 0;
    fclose(f);
  }
  DeleteFileW(request.c_str());
  path[strcspn(path, "\r\n")] = 0;
  if (!path[0]) {
    WideCharToMultiByte(CP_UTF8, 0, (g_moduleDir + L"\\remix_screenshot.png").c_str(), -1, path, sizeof(path),
                        nullptr, nullptr);
  }
  g_api.RequestPresentedScreenshot(path);
}

bool startRemix() {
  wchar_t path[MAX_PATH];
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&startRemix), &self);
  GetModuleFileNameW(self, path, MAX_PATH);
  std::wstring dll = path;
  g_moduleDir = dll.substr(0, dll.find_last_of(L"\\/"));
  // OpenRemix runs in process (GMod is x64): bin/win64/openremix/remix.dll,
  // its dependencies (SDL3, OpenUSD, DLSS) resolved from that directory.
  dll = g_moduleDir + L"\\openremix\\remix.dll";
  if (!LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH))
    return false;
  remixapi_HMODULE lib = nullptr;
  if (remixapi_lib_loadRemixDllAndInitialize(dll.c_str(), &g_api, &lib) != REMIXAPI_ERROR_CODE_SUCCESS)
    return false;
  remixapi_StartupInfo startup = {};
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
  startup.hwnd = g_gameWindow;
  if (g_api.Startup(&startup) != REMIXAPI_ERROR_CODE_SUCCESS)
    return false;
  remix_ui::install(g_gameWindow, &g_api);
  // OPENREMIX_PROXY_BACKEND=pathtrace|raster (runtime default: raster).
  char backend[32] = {};
  if (GetEnvironmentVariableA("OPENREMIX_PROXY_BACKEND", backend, sizeof(backend)))
    g_api.SetConfigVariable("rtx.native.backend", backend);
  // USD mod replacements from the game's rtx-remix/mods (OPENREMIX_PROXY_MODS:
  // another directory, or 0 to disable). Z up.
  {
    char mods[MAX_PATH * 2] = {};
    if (!GetEnvironmentVariableA("OPENREMIX_PROXY_MODS", mods, sizeof(mods))) {
      GetModuleFileNameA(nullptr, mods, MAX_PATH);
      *strrchr(mods, '\\') = 0;
      strcat(mods, "\\rtx-remix\\mods");
    }
    auto configure = reinterpret_cast<decltype(&remixapi_ConfigureUSDModsEXT)>(
        GetProcAddress(GetModuleHandleW(L"remix.dll"), "remixapi_ConfigureUSDModsEXT"));
    if (configure && std::strcmp(mods, "0") != 0) {
      remixapi_USDModsOptionsEXT options = {};
      options.structSize = sizeof(options);
      options.version = 1;
      options.directory = mods;
      // Remix captures and mods declare metersPerUnit 0.01 but hold game units,
      // which dxvk-remix applies unscaled: match the declaration.
      options.hostMetersPerUnit = 0.01;
      options.hostUpAxis = 2;
      char watch[8] = {};
      options.watch = !(GetEnvironmentVariableA("OPENREMIX_PROXY_MODS_WATCH", watch, sizeof(watch)) && watch[0] == '0');
      g_modsResult = configure(&options);
    }
  }
  {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (wchar_t *slash = wcsrchr(exe, L'\\'))
      *slash = 0;
    categories::load(exe);
  }
  // Source units are inches: the runtime's default local light reach (16
  // units, meant as meters) would fade flashlights and lamps past ~40 cm.
  g_api.SetConfigVariable("rtx.native.pathtrace.localRange", "0");
  // The adaptive local-light budget compares absolute importance against
  // thresholds tuned for meter-scale scenes; with Source's light intensities
  // nearly every pixel takes the maximum 4 shadow rays (~10 ms on c8m1).
  g_api.SetConfigVariable("rtx.native.pathtrace.adaptiveLocalSampling", "0");
  // Hundreds of mod lights, mostly behind walls, would otherwise take most
  // samples from a nearby flashlight: RIS over 4 candidates weighs their
  // unshadowed contributions (spot cones included) before the shadow ray.
  g_api.SetConfigVariable("rtx.native.pathtrace.sphereCandidates", "4");
  // Source units are inches: the runtime converts its meter-based defaults
  // (denoiser hit distances, visibility-guiding cells) with the scene scale.
  g_api.SetConfigVariable("rtx.native.pathtrace.sceneScale", "0.0254");
  // Hundreds of mod lights, mostly behind walls: reservoir reuse (ReSTIR DI)
  // keeps the visible samples instead of re-drawing from the light tree.
  g_api.SetConfigVariable("rtx.native.pathtrace.restir", "1");
  // Static world brushes (onPresent) would otherwise make every ray traverse
  // a static and a dynamic acceleration structure.
  g_api.SetConfigVariable("rtx.native.pathtrace.combinedTlas", "1");
  // OPENREMIX_PROXY_CONFIG="key=value;key=value": runtime settings at startup.
  char config[2048] = {};
  if (GetEnvironmentVariableA("OPENREMIX_PROXY_CONFIG", config, sizeof(config))) {
    for (char *entry = strtok(config, ";"); entry; entry = strtok(nullptr, ";")) {
      if (char *eq = strchr(entry, '=')) {
        *eq = 0;
        g_api.SetConfigVariable(entry, eq + 1);
      }
    }
  }
  return true;
}

} // namespace

bool enabled() {
  static const bool on = [] {
    char v[8] = {};
    return !(GetEnvironmentVariableA("OPENREMIX_PROXY", v, sizeof(v)) && v[0] == '0');
  }();
  return on;
}

void setShaderApi(void *real) { g_shaderApi = real; }

bool viewMatrices(float view[16], float projection[16]) {
  if (!g_shaderApi)
    return false;
  getMatrix(kMatrixView, view);
  getMatrix(kMatrixProjection, projection);
  return true;
}

bool isSkyDraw(const Draw &d) {
  return g_ready && g_worldLists > 0 && d.mainTarget && !d.blend && d.streams[0].dynamic && d.colorWrite;
}

// --- Sky dome ----------------------------------------------------------------
// The captured 2D skybox, resampled to the dome's equirectangular layout
// (u = atan2(z, x) / 2pi + 0.5, v = acos(y) / pi in dome space) and lit at
// OPENREMIX_PROXY_SKY_DOME_SCALE. The dome transform swaps y and z, so the
// poles (rows that collapse to a point) are Source's up and down and the
// seam is a meridian; with the identity they lay on the horizon at +-y. The
// swap is its own inverse: either reading of the transform gives the same map.
remixapi_LightHandle g_skyDome = nullptr;
remixapi_TextureHandle g_skyTexture = nullptr;
uint64_t g_skyTextureHash = 0;
uint32_t g_skyEpoch = 0;

uint32_t skyEpoch() { return g_skyEpoch; }

// A new map: the dome of the previous one must not outlive it (a map whose
// 2D sky is not drawn, or not yet, falls back to the sky light estimate).
static void resetSky() {
  if (g_skyDome)
    g_api.DestroyLight(g_skyDome);
  if (g_skyTexture)
    g_api.DestroyTexture(g_skyTexture);
  g_skyDome = nullptr;
  g_skyTexture = nullptr;
  g_skyTextureHash = 0;
  ++g_skyEpoch;
}

bool setSkyFaces(const uint8_t *const faces[6]) {
  if (!g_ready)
    return false;
  constexpr uint32_t kWidth = proxy::sky::kDomeWidth, kHeight = proxy::sky::kDomeHeight, S = proxy::sky::kFaceSize;
  // The faces identify the dome: unchanged ones cost neither the resampling
  // nor a texture upload.
  uint64_t hash = kWidth;
  for (int f = 0; f < 6; ++f)
    hash = XXH3_64bits_withSeed(faces[f], size_t(S) * S * 4, hash);
  hash |= 1;
  if (hash == g_skyTextureHash)
    return false;
  float bases[6][3][3];
  for (int f = 0; f < 6; ++f)
    proxy::sky::faceBasis(f, bases[f][0], bases[f][1], bases[f][2]);
  std::vector<float> cosPhi(kWidth), sinPhi(kWidth);
  for (uint32_t u = 0; u < kWidth; ++u) {
    const float phi = ((u + 0.5f) / kWidth - 0.5f) * 6.28318531f;
    cosPhi[u] = std::cos(phi);
    sinPhi[u] = std::sin(phi);
  }
  static std::vector<uint8_t> rgba; // kept (8 MiB)
  rgba.resize(size_t(kWidth) * kHeight * 4);
  auto rows = [&](uint32_t begin, uint32_t end) {
    for (uint32_t v = begin; v < end; ++v) {
      const float theta = (v + 0.5f) / kHeight * 3.14159265f;
      const float sinTheta = std::sin(theta), cosTheta = std::cos(theta);
      uint8_t *out = &rgba[size_t(v) * kWidth * 4];
      for (uint32_t u = 0; u < kWidth; ++u, out += 4) {
        // Dome direction (sin cos, cos, sin sin) with y and z swapped: world.
        const float w[3] = {sinTheta * cosPhi[u], sinTheta * sinPhi[u], cosTheta};
        // The face of the major axis (faceBasis order: +x -x +y -y +z -z).
        const float ax = std::fabs(w[0]), ay = std::fabs(w[1]), az = std::fabs(w[2]);
        const int axis = ax >= ay && ax >= az ? 0 : ay >= az ? 1 : 2;
        const int face = axis * 2 + (w[axis] < 0);
        const float inv = 1 / std::max(std::fabs(w[axis]), 1e-6f);
        const float x = (w[0] * bases[face][0][0] + w[1] * bases[face][0][1] + w[2] * bases[face][0][2]) * inv;
        const float y = (w[0] * bases[face][1][0] + w[1] * bases[face][1][1] + w[2] * bases[face][1][2]) * inv;
        // Bilinear, texel centres at half-integers (the capture offsets its
        // projection to match); the last half texel at a face's edge clamps.
        const float px = std::clamp((x * 0.5f + 0.5f) * S - 0.5f, 0.f, S - 1.001f);
        const float py = std::clamp((0.5f - y * 0.5f) * S - 0.5f, 0.f, S - 1.001f);
        const uint32_t x0 = uint32_t(px), y0 = uint32_t(py);
        const float fx = px - x0, fy = py - y0;
        const uint8_t *t0 = faces[face] + (size_t(y0) * S + x0) * 4, *t1 = t0 + size_t(S) * 4;
        for (int c = 0; c < 3; ++c) {
          const int k = 2 - c; // BGRA -> RGBA
          const float value = (t0[k] * (1 - fx) + t0[4 + k] * fx) * (1 - fy) + (t1[k] * (1 - fx) + t1[4 + k] * fx) * fy;
          out[c] = uint8_t(value + 0.5f);
        }
        out[3] = 255;
      }
    }
  };
  {
    // 2M texels: split over a few threads.
    const uint32_t workers = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
    std::vector<std::thread> threads;
    for (uint32_t i = 1; i < workers; ++i)
      threads.emplace_back(rows, kHeight * i / workers, kHeight * (i + 1) / workers);
    rows(0, kHeight / workers);
    for (auto &t : threads)
      t.join();
  }
  remixapi_TextureInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
  info.hash = hash;
  info.width = kWidth;
  info.height = kHeight;
  info.depth = 1;
  info.mipLevels = 1;
  info.format = REMIXAPI_FORMAT_R8G8B8A8_SRGB;
  info.data = rgba.data();
  info.dataSize = rgba.size();
  remixapi_TextureHandle texture = nullptr;
  if (g_api.CreateTexture(&info, &texture) != REMIXAPI_ERROR_CODE_SUCCESS)
    return false;
  static const float domeScale = envScale("OPENREMIX_PROXY_SKY_DOME_SCALE", 1.0f);
  wchar_t path[32];
  std::swprintf(path, 32, L"0x%llx", static_cast<unsigned long long>(hash));
  remixapi_LightInfoDomeEXT dome = {};
  dome.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT;
  dome.transform = {{{1, 0, 0, 0}, {0, 0, 1, 0}, {0, 1, 0, 0}}};
  dome.colorTexture = path;
  remixapi_LightInfo light = {};
  light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  light.pNext = &dome;
  light.hash = 0x5b7d0e0000000001ull;
  light.radiance = {domeScale, domeScale, domeScale};
  remixapi_LightHandle handle = nullptr;
  if (g_api.CreateLight(&light, &handle) != REMIXAPI_ERROR_CODE_SUCCESS) {
    g_api.DestroyTexture(texture);
    return false;
  }
  if (g_skyDome && g_skyDome != handle)
    g_api.DestroyLight(g_skyDome);
  if (g_skyTexture && g_skyTexture != texture)
    g_api.DestroyTexture(g_skyTexture);
  g_skyDome = handle;
  g_skyTexture = texture;
  g_skyTextureHash = hash;
  return true;
}

void onCreateDevice(HWND gameWindow, D3DPRESENT_PARAMETERS *pp) {
  if (!enabled() || !pp)
    return;
  if (gameWindow || !g_gameWindow)
    g_gameWindow = pp->hDeviceWindow ? pp->hDeviceWindow : gameWindow;
  g_displayWidth = pp->BackBufferWidth;
  g_displayHeight = pp->BackBufferHeight;
  // D3D9 keeps rendering (to a hidden window) so the shader API is unaffected;
  // the game window belongs to the Remix swapchain.
  if (!g_hiddenWindow) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = hiddenProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"shaderapi_proxy_hidden";
    RegisterClassW(&wc);
    g_hiddenWindow = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, pp->BackBufferWidth,
                                     pp->BackBufferHeight, nullptr, nullptr, wc.hInstance, nullptr);
  }
  pp->hDeviceWindow = g_hiddenWindow;
  pp->Windowed = TRUE;
  pp->FullScreen_RefreshRateInHz = 0;
}

void onDeviceCreated(IDirect3DDevice9 *) {
  if (!enabled() || g_ready)
    return;
  g_ready = startRemix();
}

void onDraw(const Draw &d) {
  Timer timer(g_usDraw);
  ++g_stats.draws;
  if (!g_ready || !g_shaderApi)
    return;
  if (!d.mainTarget) {
    ++g_stats.offTarget;
    return;
  }
  if ((d.colorWrite & 7) == 0) {
    ++g_stats.depthOnly; // depth prepass / shadow depth
    return;
  }
  uint32_t categories = remixCategories(d.forkCategories) | g_markerCategories;
  // rtx.conf texture categories, by the base texture's dxvk hash.
  const uint64_t baseTextureHash = textureHash(d.texture0);
  categories |= categories::lookup(baseTextureHash);
  if (categories & (REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE | REMIXAPI_INSTANCE_CATEGORY_BIT_HIDDEN)) {
    ++g_stats.ignored;
    return;
  }
  // Source depth-biases decals and overlays. Alpha-over and multiplicative
  // ones (blood and dirt: DESTCOLOR*SRCCOLOR) paint the surface below; the
  // runtime needs them lifted off it, since only D3D applied the bias.
  const bool multiply = (d.srcBlend == D3DBLEND_DESTCOLOR && (d.dstBlend == D3DBLEND_SRCCOLOR || d.dstBlend == D3DBLEND_ZERO)) ||
                        (d.srcBlend == D3DBLEND_ZERO && d.dstBlend == D3DBLEND_SRCCOLOR);
  constexpr uint32_t kDecalCategories =
      REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC | REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_DYNAMIC |
      REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_SINGLE_OFFSET | REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_NO_OFFSET;
  const bool decal = d.blend && d.blendOp == D3DBLENDOP_ADD &&
                     ((d.srcBlend == D3DBLEND_SRCALPHA && d.dstBlend == D3DBLEND_INVSRCALPHA) || multiply) &&
                     (d.depthBias || (categories & kDecalCategories));
  if (decal && !(categories & kDecalCategories))
    categories |= REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC;
  // Dev aid (OPENREMIX_PROXY_DUMP_DECALS=1): each blended draw's state, once
  // per texture, to bin/decals.txt.
  static const bool dumpDecals = [] {
    char v[8] = {};
    return GetEnvironmentVariableA("OPENREMIX_PROXY_DUMP_DECALS", v, sizeof(v)) && v[0] == '1';
  }();
  if (dumpDecals && d.blend) {
    static std::unordered_set<uint64_t> seen;
    if (seen.insert(baseTextureHash ^ (uint64_t(d.srcBlend) << 56) ^ (uint64_t(d.dstBlend) << 48)).second)
      if (FILE *f = _wfopen((g_moduleDir + L"\\decals.txt").c_str(), L"a")) {
        fprintf(f, "tex %016llX %s decal %d multiply %d src %u dst %u op %u sepA %d srcA %u dstA %u depthBias %d alphaTest %d ref %u "
                   "cats %08X color %d write %X cull %u\n",
                (unsigned long long)baseTextureHash, bsp::materialName(baseTextureHash).c_str(), int(decal), int(multiply),
                d.srcBlend, d.dstBlend, d.blendOp, int(d.separateAlpha), d.srcBlendAlpha, d.dstBlendAlpha,
                int(d.depthBias), int(d.alphaTest), d.alphaRef, categories,
                int(findElement(d.elements, D3DDECLUSAGE_COLOR, 0) != nullptr), d.colorWrite, d.cullMode);
        fclose(f);
      }
  }
  constexpr float kDecalOffset = 0.1f; // inches
  if (categories & (REMIXAPI_INSTANCE_CATEGORY_BIT_THIRD_PERSON_PLAYER_BODY |
                    REMIXAPI_INSTANCE_CATEGORY_BIT_THIRD_PERSON_PLAYER_MODEL)) {
    // l4d2-rtx's player body for first-person shadows. The runtime has no
    // shadow-only instances yet, so keep it out of view.
    ++g_stats.playerBody;
    return;
  }
  // Blended draws (particles, sprites, glass, overlays) keep their D3D blend
  // state; extra passes over already drawn geometry are dropped as duplicates.
  if (d.blend)
    ++(d.srcBlend == D3DBLEND_ONE && d.dstBlend == D3DBLEND_ONE ? g_stats.additive : g_stats.blended);
  const auto *pos = findElement(d.elements, D3DDECLUSAGE_POSITION, 0);
  if (!pos || pos->Type != D3DDECLTYPE_FLOAT3) {
    ++g_stats.noPosition;
    return;
  }
  uint32_t bonesPerVertex = 0;
  if (findElement(d.elements, D3DDECLUSAGE_BLENDINDICES, 0)) {
    bonesPerVertex = uint32_t(std::clamp(currentNumBones(), 1, 3));
    ++g_stats.skinned;
  }
  const auto &ps = d.streams[pos->Stream];
  if (!ps.data || !d.indices.data) {
    ++g_stats.noData;
    return;
  }

  const uint64_t viewKey = fnv(d.vsConstants + kRegViewProj * 4, 16 * sizeof(float));
  auto &view = g_views[viewKey];
  if (view.draws++ == 0) {
    view.order = g_viewOrder++;
    getMatrix(kMatrixView, view.view);
    getMatrix(kMatrixProjection, view.projection);
    // Source builds right-handed D3DX matrices (row vectors, camera looking
    // down -z, projection m[2][3] = -1, depth 0..1). Flipping view-space z
    // (negate the view's third column and the projection's third row) keeps
    // view * projection and yields the left-handed form the runtime expects.
    view.perspective = std::fabs(std::fabs(view.projection[11]) - 1.0f) < 1e-3f && std::fabs(view.projection[15]) < 1e-3f;
    if (view.perspective && view.projection[11] < 0) {
      for (int r = 0; r < 4; ++r)
        view.view[r * 4 + 2] = -view.view[r * 4 + 2];
      for (int c = 0; c < 4; ++c)
        view.projection[8 + c] = -view.projection[8 + c];
    }
  }
  if (!view.perspective) {
    ++g_stats.flat; // HUD / VGUI
    captureUi(d);
    return;
  }
  // The BSP world replaces the engine's opaque world surfaces (static vertex
  // buffers; their index buffers are rebuilt per frame); its blended draws
  // (translucent brushes, decals, overlays) and dynamic ones (the 2D skybox)
  // still come from the engine.
  // Overlays (COverlayMgr::RenderOverlays, openremix_proxy_overlays).
  if (g_overlays > 0 && bsp::overlaysActive()) {
    ++g_stats.bspOverlaysDropped;
    return;
  }
  // Static props likewise (matched by placement; translucent ones stay).
  float eye[3];
  for (int i = 0; i < 3; ++i)
    eye[i] = -(view.view[12] * view.view[i * 4] + view.view[13] * view.view[i * 4 + 1] +
               view.view[14] * view.view[i * 4 + 2]);
  if (!d.blend && bsp::isStaticProp(d.vsConstants + kRegModel * 4, eye)) {
    // Their ambient cubes still feed the sky light estimate (onPresent).
    if (findElement(d.elements, D3DDECLUSAGE_BLENDINDICES, 0))
      view.ambientUp.push_back({d.vsConstants[kRegAmbientUp * 4], d.vsConstants[kRegAmbientUp * 4 + 1],
                                d.vsConstants[kRegAmbientUp * 4 + 2]});
    ++g_stats.bspPropsDropped;
    return;
  }
  // The engine's flashlight pass over the world: with a projected texture on,
  // it re-draws the lit surfaces additively with the cookie as texture 0
  // (captured, the cookie got pasted over every surface). Those lights are
  // path traced (engine_lights), so the pass goes. The shadow manager's
  // virtual iterator is not what the world pass uses, so it is caught here.
  // Models get the same additive pass outside the world lists, where other
  // additive draws are legitimate (glows, sprites). Two signs of the pass:
  // the cookie as texture 0 (world-style shaders; the world pass teaches
  // which textures are cookies), or, for models, whose pass keeps the base
  // texture there, the same buffers and range as an opaque draw earlier in
  // the frame (effects are drawn once). Additive is ONE/ONE, or
  // SRCALPHA/ONE for translucent materials (decals).
  struct {
    const void *vertices, *indexBuffer;
    uint32_t offset, startIndex, primCount, minIndex;
    int baseVertex;
    uint32_t padding; // hashed as bytes: no indeterminate padding
    uint64_t view;
  } drawRange = {ps.buffer, d.indices.buffer, ps.offset, d.startIndex, d.primCount, d.minIndex, d.baseVertex, 0, viewKey};
  static std::unordered_set<uint64_t> opaqueDrawn; // render thread only
  static uint64_t opaqueFrame = ~0ull;
  if (opaqueFrame != g_frame) {
    opaqueDrawn.clear();
    opaqueFrame = g_frame;
  }
  const uint64_t geometryKey = fnv(&drawRange, sizeof(drawRange));
  if (!d.blend && !ps.dynamic)
    opaqueDrawn.insert(geometryKey);
  if (d.blend && d.dstBlend == D3DBLEND_ONE && (d.srcBlend == D3DBLEND_ONE || d.srcBlend == D3DBLEND_SRCALPHA)) {
    static std::unordered_set<const void *> cookies; // render thread only
    static uint32_t cookieEpoch = ~0u;
    if (cookieEpoch != g_skyEpoch) { // a new map: texture pointers are reused
      cookies.clear();
      cookieEpoch = g_skyEpoch;
    }
    if (const auto lights = engine_lights::current(); lights && !lights->projected.empty()) {
      if (g_worldLists > 0 && d.texture0)
        cookies.insert(d.texture0);
      if (g_worldLists > 0 || cookies.count(d.texture0) || (!ps.dynamic && opaqueDrawn.count(geometryKey))) {
        ++g_stats.flashlightPasses;
        return;
      }
    }
  }
  if (g_worldLists > 0 && bsp::active()) {
    if (!d.blend && !ps.dynamic) {
      ++g_stats.bspDropped;
      return;
    }
    ++(d.blend ? g_stats.bspKeptBlended : g_stats.bspKeptDynamic);
    if (!d.blend) {
      std::lock_guard<std::mutex> lock(g_transientSampleLock);
      ++g_keptWorldTextures[baseTextureHash];
    }
  }

  struct Key {
    const void *vb, *ib, *decl;
    uint32_t vbVersion, ibVersion, offset, stride;
    int baseVertex;
    uint32_t minIndex, numVertices, startIndex, primCount, type, bonesPerVertex;
  } key = {ps.buffer, d.indices.buffer, d.declaration, ps.version, d.indices.version, ps.offset, ps.stride,
           d.baseVertex, d.minIndex, d.numVertices, d.startIndex, d.primCount, uint32_t(d.type), bonesPerVertex};
  const uint64_t hash = fnv(&key, sizeof(key)) ^ (decal ? 0xdeca1deca1deca1ull : 0);

  remixapi_MeshHandle mesh = nullptr;
  uint32_t boneCount = 0;
  uint64_t geometry = 0;
  if (ps.dynamic || d.indices.dynamic) {
    // Dynamic buffers are rewritten every frame: build, draw once, destroy.
    g_transientIndices += d.primCount * 3;
    {
      std::lock_guard<std::mutex> lock(g_transientSampleLock);
      ++g_transientDecls[d.declaration];
    }
    mesh = buildMesh(d, hash, bonesPerVertex, &boneCount, decal ? kDecalOffset : 0);
    if (!mesh)
      return;
    g_transient.push_back(mesh);
    // The replacement key only matters to mods.
    if (g_modsResult == REMIXAPI_ERROR_CODE_SUCCESS)
      geometry = dxvk::geometryHash(d);
  } else if (auto it = g_meshes.find(hash); it != g_meshes.end()) {
    mesh = it->second.handle;
    boneCount = it->second.bones;
    geometry = it->second.geometry;
    it->second.lastFrame = g_frame;
    ++g_stats.cacheHits;
  } else {
    mesh = buildMesh(d, hash, bonesPerVertex, &boneCount, decal ? kDecalOffset : 0);
    if (!mesh)
      return;
    geometry = dxvk::geometryHash(d);
    g_meshes[hash] = {mesh, g_frame, boneCount, geometry};
  }

  Instance inst;
  inst.mesh = mesh;
  inst.material = d.texture0 ? materialFor(d.texture0) : nullptr;
  inst.blend = blendState(d);
  // Experiment (OPENREMIX_PROXY_CULL_BLENDED=1): blended surfaces keep D3D's
  // culling (single-sided instances). Off by default: the runtime's front-face
  // winding has not been checked against D3D's, and on openremix-testing it did
  // not change the transparent layer count or cost.
  static const bool cullBlended = [] {
    char v[8] = {};
    return GetEnvironmentVariableA("OPENREMIX_PROXY_CULL_BLENDED", v, sizeof(v)) && v[0] == '1';
  }();
  inst.singleSided = cullBlended && d.blend && d.cullMode != D3DCULL_NONE;
  inst.categories = categories;
  inst.replacementHash = geometry ? geometry ^ baseTextureHash : 0;
  inst.persistent = !(ps.dynamic || d.indices.dynamic);
  noteReplacementKey(inst.replacementHash);
  inst.texture0 = d.texture0;
  if (boneCount) {
    // Bone matrices map the bind pose straight to world space.
    inst.bones.resize(boneCount);
    std::memcpy(inst.bones.data(), d.vsConstants + kRegModel * 4, boneCount * sizeof(remixapi_Transform));
    static const float identity[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    std::memcpy(inst.transform, identity, sizeof(inst.transform));
  } else {
    std::memcpy(inst.transform, d.vsConstants + kRegModel * 4, sizeof(inst.transform));
  }
  inst.view = viewKey;
  g_instances.push_back(inst);
  if (boneCount) // studio models: their ambient cube samples the surrounding (sky) light
    view.ambientUp.push_back({d.vsConstants[kRegAmbientUp * 4], d.vsConstants[kRegAmbientUp * 4 + 1],
                              d.vsConstants[kRegAmbientUp * 4 + 2]});
  ++g_stats.accepted;
}

void onSetLights(int count, const void *lights) {
  if (!g_ready || !lights || count <= 0 || count > 8)
    return;
  std::lock_guard<std::mutex> lock(g_lightLock);
  for (int i = 0; i < count; ++i) {
    SourceLight l;
    std::memcpy(&l, static_cast<const uint8_t *>(lights) + i * kLightDescSize, sizeof(l));
    if (l.type < 1 || l.type > 3 || bsp::providesLight(l.type, l.position, l.direction))
      continue;
    // Identity: type, color, position, direction and falloff (not derived
    // values). Directional lights carry a per-model position; they are the
    // same light for every model, so position and falloff are ignored.
    SourceLight key = l;
    if (l.type == 2) {
      std::memset(key.position, 0, sizeof(key.position));
      key.range = key.falloff = key.attenuation0 = key.attenuation1 = key.attenuation2 = key.theta = key.phi = 0;
    }
    g_frameLights[fnv(&key, offsetof(SourceLight, thetaDot))] = l;
  }
}

// Dev aid (OPENREMIX_PROXY_GPU_STATS=1): GPU pass times averaged over every
// frame between statistics dumps, instead of one frame per dump.
struct GpuAverages {
  std::mutex lock;
  std::map<std::string, double> passes;
  double frame = 0;
  uint64_t count = 0, lastFrame = ~0ull;
} g_gpuAverages;
void sampleGpuPasses() {
  static const bool enabled = [] {
    char v[8] = {};
    return GetEnvironmentVariableA("OPENREMIX_PROXY_GPU_STATS", v, sizeof(v)) && v[0] == '1';
  }();
  if (!enabled)
    return;
  static auto get = reinterpret_cast<decltype(&remixapi_GetGpuPassTimingsEXT)>(
      GetProcAddress(GetModuleHandleW(L"remix.dll"), "remixapi_GetGpuPassTimingsEXT"));
  static remixapi_GpuPassTimingsEXT t;
  t = {};
  t.structSize = sizeof(t);
  if (!get || get(&t) != REMIXAPI_ERROR_CODE_SUCCESS || !t.available)
    return;
  std::lock_guard<std::mutex> guard(g_gpuAverages.lock);
  if (t.frame == g_gpuAverages.lastFrame)
    return; // no newer measured frame yet
  g_gpuAverages.lastFrame = t.frame;
  g_gpuAverages.frame += t.frameMilliseconds;
  ++g_gpuAverages.count;
  for (uint32_t i = 0; i < t.passCount && i < REMIXAPI_GPU_PASS_TIMING_CAPACITY_EXT; ++i)
    g_gpuAverages.passes[t.passes[i].name] += t.passes[i].milliseconds;
}

void shutdown() {
  if (!g_ready)
    return;
  g_ready = false;
  remix_ui::uninstall();
  g_api.Shutdown();
}

void onPresent() {
  sampleGpuPasses();
  if (!g_ready)
    return;
  ++g_stats.frames;
  {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    if (g_lastPresent.QuadPart)
      g_usFrame += uint32_t(double(now.QuadPart - g_lastPresent.QuadPart) * 1e6 / double(freq.QuadPart));
    g_lastPresent = now;
  }
  Timer timer(g_usSubmit);
  // Classify this frame's perspective views. The main view is the one with the
  // most draws among those sharing the most common field of view. Views with
  // the main rotation are: the same camera with other depth planes (world),
  // the 3D skybox (same field of view, sky-space position) or the view model
  // (another field of view). Other views (monitors, reflections) are skipped.
  std::unordered_map<int, uint32_t> fovDraws;
  for (auto &[key, v] : g_views)
    if (v.perspective)
      fovDraws[int(std::lround(v.projection[5] * 1000))] += v.draws;
  int worldFov = 0;
  uint32_t best = 0;
  for (auto &[fov, n] : fovDraws)
    if (n > best) {
      best = n;
      worldFov = fov;
    }
  const View *main = nullptr;
  uint64_t mainKey = 0;
  for (auto &[key, v] : g_views) {
    if (v.perspective && int(std::lround(v.projection[5] * 1000)) == worldFov && (!main || v.draws > main->draws)) {
      main = &v;
      mainKey = key;
    }
  }
  // Draw counts are a weak signal (with the engine's static world skipped the
  // view model can out-draw the world, and a view may have no draws at all).
  // The engine says which views have a world: the last one drawn is the main
  // view (the 3D skybox comes first), unless sky_camera says otherwise.
  if (!g_worldViews.empty()) {
    auto eyeOf = [](const float *v, float out[3]) {
      for (int i = 0; i < 3; ++i)
        out[i] = -(v[12] * v[i * 4] + v[13] * v[i * 4 + 1] + v[14] * v[i * 4 + 2]);
    };
    const WorldView *chosen = &g_worldViews.back();
    float origin[3], scale = 0;
    if (g_worldViews.size() > 1 && bsp::skyCamera(origin, scale) && scale > 0)
      for (const auto &candidate : g_worldViews)
        for (const auto &other : g_worldViews) {
          float eye[3], skyEye[3], d2 = 0;
          eyeOf(candidate.view, eye);
          eyeOf(other.view, skyEye);
          for (int i = 0; i < 3; ++i) {
            const float d = skyEye[i] - (origin[i] + eye[i] / scale);
            d2 += d * d;
          }
          if (&candidate != &other && d2 < 16.0f)
            chosen = &candidate;
        }
    worldFov = int(std::lround(chosen->projection[5] * 1000));
    main = nullptr;
    for (auto &[key, v] : g_views)
      if (v.perspective && int(std::lround(v.projection[5] * 1000)) == worldFov &&
          samePlacement(v.view, chosen->view) && (!main || v.draws > main->draws)) {
        main = &v;
        mainKey = key;
      }
    if (!main) {
      // No engine draw in the main view this frame (the BSP world shows it all).
      constexpr uint64_t kWorldViewKey = 0x776f726c64766965ull;
      View &v = g_views[kWorldViewKey];
      v.perspective = true;
      v.order = g_viewOrder++;
      std::memcpy(v.view, chosen->view, sizeof(v.view));
      std::memcpy(v.projection, chosen->projection, sizeof(v.projection));
      main = &v;
      mainKey = kWorldViewKey;
    }
    ++g_stats.worldViewCameras;
  }
  // Draw counts can favour the 3D skybox: looking down, the engine culls most
  // of the world while the skybox view draws its whole miniature city. The
  // skybox view looks from sky_camera's origin + eye / scale with the main
  // rotation, so a winner that is another candidate's skybox view yields to it.
  float skyOrigin[3], skyScale = 0;
  if (main && bsp::skyCamera(skyOrigin, skyScale) && skyScale > 0) {
    auto eyeOf = [](const View &v, float out[3]) {
      for (int i = 0; i < 3; ++i)
        out[i] = -(v.view[12] * v.view[i * 4] + v.view[13] * v.view[i * 4 + 1] + v.view[14] * v.view[i * 4 + 2]);
    };
    float mainEye[3];
    eyeOf(*main, mainEye);
    for (auto &[key, v] : g_views) {
      if (&v == main || !v.perspective || int(std::lround(v.projection[5] * 1000)) != worldFov ||
          !sameRotation(v.view, main->view))
        continue;
      float eye[3], distance2 = 0;
      eyeOf(v, eye);
      for (int i = 0; i < 3; ++i) {
        const float d = mainEye[i] - (skyOrigin[i] + eye[i] / skyScale);
        distance2 += d * d;
      }
      if (distance2 < 4.0f * 4.0f) {
        main = &v;
        mainKey = key;
        ++g_stats.skyViewDemoted;
        break;
      }
    }
  }
  if (main) {
    g_lastCamera = *main;
    // The 3D skybox keeps its own camera: the runtime draws SKY instances with
    // the sky camera behind the world (raster layer 0, path-traced
    // background), as Source does.
    enum class Role { Skip, Main, Sky, ViewModel };
    std::unordered_map<uint64_t, Role> roles;
    const View *viewModel = nullptr, *sky = nullptr;
    for (auto &[key, v] : g_views) {
      Role role = Role::Skip;
      if (key == mainKey) {
        role = Role::Main;
      } else if (v.perspective && samePlacement(v.view, main->view) &&
                 int(std::lround(v.projection[5] * 1000)) == worldFov) {
        role = Role::Main; // drawn with the main camera
      } else if (v.perspective && sameRotation(v.view, main->view)) {
        const bool worldFovView = int(std::lround(v.projection[5] * 1000)) == worldFov;
        role = worldFovView ? Role::Sky : Role::ViewModel;
        const View *&chosen = worldFovView ? sky : viewModel;
        if (!chosen || v.draws > chosen->draws)
          chosen = &v;
      }
      roles[key] = role;
    }
    if (sky)
      ++g_stats.skyFrames;
    g_viewSummary.clear();
    for (auto &[key, v] : g_views) {
      char line[160];
      static const char *const names[] = {"skip", "main", "sky", "viewmodel"};
      snprintf(line, sizeof(line), "    view order %u draws %u persp %d P0 %.3f P5 %.3f P14 %.3f T %.0f %.0f %.0f -> %s\n",
               v.order, v.draws, v.perspective, v.projection[0], v.projection[5], v.projection[14], v.view[12],
               v.view[13], v.view[14], names[int(roles[key])]);
      g_viewSummary += line;
    }

    // Sky light: the path tracer's environment radiance follows the upward
    // face of the ambient cubes Source computes for lit models in the main
    // view (median per frame, smoothed over time).
    static float skyRadiance[3] = {};
    static bool skyKnown = false;
    static const float skyScale = envScale("OPENREMIX_PROXY_SKY_SCALE", 1.0f);
    if (!main->ambientUp.empty()) {
      auto samples = main->ambientUp;
      std::sort(samples.begin(), samples.end(),
                [](const auto &a, const auto &b) { return a[0] + a[1] + a[2] < b[0] + b[1] + b[2]; });
      const auto &median = samples[samples.size() / 2];
      for (int i = 0; i < 3; ++i) {
        const float target = std::isfinite(median[i]) ? std::max(median[i], 0.0f) * skyScale : 0;
        skyRadiance[i] = skyKnown ? skyRadiance[i] + (target - skyRadiance[i]) * 0.05f : target;
      }
      skyKnown = true;
    }
    g_lastSky[0] = skyRadiance[0];
    g_lastSky[1] = skyRadiance[1];
    g_lastSky[2] = skyRadiance[2];
    remixapi_CameraEnvironmentEXT environment = {};
    environment.sType = REMIXAPI_STRUCT_TYPE_CAMERA_ENVIRONMENT_EXT;
    environment.enabled = skyKnown;
    environment.radiance = {skyRadiance[0], skyRadiance[1], skyRadiance[2]};

    remixapi_CameraInfo camera = {};
    camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
    auto setup = [&](remixapi_CameraType type, const View &v) {
      camera.pNext = type == REMIXAPI_CAMERA_TYPE_WORLD ? &environment : nullptr;
      camera.type = type;
      std::memcpy(camera.view, v.view, sizeof(camera.view));
      std::memcpy(camera.projection, v.projection, sizeof(camera.projection));
      g_api.SetupCamera(&camera);
    };
    // The view model is path-traced as world geometry: scaled in view space so
    // the main camera projects it like Source's view-model camera, and the
    // world camera's near plane moves in to keep it (a few units from the eye)
    // unclipped. OPENREMIX_PROXY_RASTER_VIEWMODEL=1 keeps the separate
    // rasterized view-model camera instead.
    static const bool rasterViewModel = envScale("OPENREMIX_PROXY_RASTER_VIEWMODEL", 0) > 0;
    View world = *main;
    float viewModelTransform[3][4] = {};
    if (viewModel && !rasterViewModel) {
      viewScaleTransform(main->view, viewModel->projection[0] / main->projection[0],
                         viewModel->projection[5] / main->projection[5], viewModelTransform);
      // Move only the near plane: p10 and p14 together define both planes
      // (LH, depth 0..1: n = -p14/p10, f = p14/(1-p10)); changing p14 alone
      // also pulled the far plane in (~35000 -> ~5000 units), clipping
      // distant geometry and desynchronizing NRD's depth reconstruction.
      constexpr float kNear = 1.0f;
      const float p10 = world.projection[10], p14 = world.projection[14];
      const float nearPlane = -p14 / p10;
      if (nearPlane > kNear && p10 > 1.0f) {
        const float farPlane = p14 / (1.0f - p10);
        world.projection[10] = farPlane / (farPlane - kNear);
        world.projection[14] = -kNear * world.projection[10];
      }
    }
    // A path-traced 3D skybox lies far beyond the map (scaled up around it):
    // the far plane (raster primary visibility, depth) must reach it.
    if (const float reach = bsp::skyTracedReach(); reach > 0) {
      const float p10 = world.projection[10], p14 = world.projection[14];
      if (p10 > 1.0f) {
        const float nearPlane = -p14 / p10, farPlane = p14 / (1.0f - p10), wanted = 2.0f * reach + 4096.0f;
        if (farPlane < wanted && nearPlane > 0) {
          world.projection[10] = wanted / (wanted - nearPlane);
          world.projection[14] = -nearPlane * world.projection[10];
        }
      }
    }
    setup(REMIXAPI_CAMERA_TYPE_WORLD, world);
    // Without engine draws in the 3D skybox view (its world comes from the
    // BSP), the sky camera is the main camera moved into the skybox:
    // eye_sky = sky origin + eye / scale, same rotation.
    View skyFromMain;
    float skyCameraOrigin[3], skyCameraScale;
    if (!sky && bsp::skyCamera(skyCameraOrigin, skyCameraScale)) {
      skyFromMain = *main;
      const float *v = main->view;
      float eye[3];
      for (int i = 0; i < 3; ++i)
        eye[i] = -(v[12] * v[i * 4] + v[13] * v[i * 4 + 1] + v[14] * v[i * 4 + 2]);
      for (int i = 0; i < 3; ++i)
        eye[i] = skyCameraOrigin[i] + eye[i] / skyCameraScale;
      for (int j = 0; j < 3; ++j)
        skyFromMain.view[12 + j] = -(eye[0] * v[j] + eye[1] * v[4 + j] + eye[2] * v[8 + j]);
      sky = &skyFromMain;
    }
    if (sky)
      setup(REMIXAPI_CAMERA_TYPE_SKY, *sky);
    if (viewModel && rasterViewModel)
      setup(REMIXAPI_CAMERA_TYPE_VIEW_MODEL, *viewModel);

    // Stable identities for models and moving props, so the runtime keeps
    // their previous transforms and bones (motion vectors, denoiser history).
    // Source does not say which entity a draw belongs to: every instance of a
    // cached mesh takes over the id of the nearest instance of that mesh in
    // the previous frame (root bone or transform translation, within 64
    // units); the rest get new ids.
    struct Tracked {
      uint64_t id;
      float position[3];
      bool taken;
    };
    static std::unordered_map<uint64_t, std::vector<Tracked>> tracked;
    static uint64_t nextTrackedId = 1;
    std::unordered_map<uint64_t, std::vector<Tracked>> nextTracked;
    auto trackedId = [&](uint64_t group, const float position[3]) {
      constexpr float kMaxDistance2 = 64.0f * 64.0f;
      auto found = tracked.find(group);
      Tracked *best = nullptr;
      float bestDistance2 = kMaxDistance2;
      if (found != tracked.end())
        for (auto &candidate : found->second) {
          if (candidate.taken)
            continue;
          float d2 = 0;
          for (int i = 0; i < 3; ++i)
            d2 += (candidate.position[i] - position[i]) * (candidate.position[i] - position[i]);
          if (d2 < bestDistance2) {
            bestDistance2 = d2;
            best = &candidate;
          }
        }
      uint64_t id;
      if (best) {
        best->taken = true;
        id = best->id;
      } else {
        id = 0x7000000000000000ull | nextTrackedId++; // static ids are 64-bit hashes: collisions are negligible
      }
      nextTracked[group].push_back({id, {position[0], position[1], position[2]}, false});
      return id;
    };
    std::unordered_map<uint64_t, bool> seen; // multipass draws: the first pass wins
    // A path-traced 3D skybox comes from the BSP: the sky view's draws repeat it.
    const bool skyTraced = bsp::skyTracedReach() > 0;
    for (auto &inst : g_instances) {
      const Role role = roles[inst.view];
      if (role == Role::Skip)
        continue;
      if ((role == Role::ViewModel && &g_views[inst.view] != viewModel) ||
          (role == Role::Sky && (&g_views[inst.view] != sky || skyTraced)))
        continue;
      if (role == Role::ViewModel && !rasterViewModel) {
        if (inst.bones.empty())
          premultiply(viewModelTransform, inst.transform);
        else
          for (auto &bone : inst.bones)
            premultiply(viewModelTransform, bone.matrix);
      }
      struct {
        remixapi_MeshHandle mesh;
        float transform[12];
      } id = {inst.mesh, {}};
      std::memcpy(id.transform, inst.transform, sizeof(id.transform));
      uint64_t idHash = fnv(&id, sizeof(id));
      if (!inst.bones.empty())
        idHash = fnv(inst.bones.data(), inst.bones.size() * sizeof(remixapi_Transform), idHash);
      if (!seen.emplace(idHash, true).second) {
        ++g_stats.duplicate;
        continue;
      }
      if (role == Role::Sky && inst.texture0 && g_skyBrightness != 1.0f)
        inst.material = materialFor(inst.texture0, g_skyBrightness);
      remixapi_InstanceInfoMaterialOverrideEXT material = {};
      material.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_MATERIAL_OVERRIDE_EXT;
      material.material = inst.material;
      remixapi_InstanceInfoBoneTransformsEXT bones = {};
      bones.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT;
      bones.boneTransforms_values = inst.bones.data();
      bones.boneTransforms_count = uint32_t(inst.bones.size());
      material.pNext = inst.bones.empty() ? nullptr : &bones;
      remixapi_InstanceInfoBlendEXT blend = inst.blend;
      blend.pNext = inst.material ? static_cast<void *>(&material)
                                  : (inst.bones.empty() ? nullptr : static_cast<void *>(&bones));
      remixapi_InstanceInfoReplacementHashEXT replacement = {};
      replacement.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_REPLACEMENT_HASH_EXT;
      replacement.pNext = &blend;
      replacement.meshHash = inst.replacementHash;
      // World brushes (static buffers, identity transform, opaque) are static
      // instances: the runtime keeps them in its static acceleration structure
      // (with rtx.native.pathtrace.combinedTlas, set at startup, one traversal
      // per ray) and they can receive its radiance cache. ~1.5 ms of opaque
      // tracing on c8m1. OPENREMIX_PROXY_STATIC_WORLD=0 submits them dynamic.
      static const bool staticWorld = [] {
        char v[8] = {};
        return !(GetEnvironmentVariableA("OPENREMIX_PROXY_STATIC_WORLD", v, sizeof(v)) && v[0] == '0');
      }();
      static const float identityTransform[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
      remixapi_InstanceIdentityEXT identity = {};
      identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
      identity.pNext = inst.replacementHash ? static_cast<void *>(&replacement) : static_cast<void *>(&blend);
      identity.instanceId = idHash ? idHash : 1;
      identity.classification = REMIXAPI_INSTANCE_STATIC_EXT;
      identity.rasterVisible = 1;
      const bool isStatic = staticWorld && role == Role::Main && inst.persistent && inst.bones.empty() &&
                            !inst.blend.alphaBlendEnabled && !inst.categories &&
                            std::memcmp(inst.transform, identityTransform, sizeof(identityTransform)) == 0;
      // Additive draws (destination factor ONE: glows, lens flares, sparks)
      // have no surface to trace; the path tracer would show them as opaque
      // sheets. They are rasterized over the traced image with their D3D
      // blend state instead.
      const bool isAdditive = inst.blend.alphaBlendEnabled && inst.blend.dstColorBlendFactor == 1;
      const bool isTracked = !isStatic && !isAdditive && inst.persistent && role != Role::Sky;
      if (isAdditive) {
        identity.instanceId = 0;
        identity.classification = REMIXAPI_INSTANCE_RASTER_ONLY_EXT;
      }
      if (isTracked) {
        float position[3];
        for (int r = 0; r < 3; ++r)
          position[r] = inst.bones.empty() ? inst.transform[r][3] : inst.bones[0].matrix[r][3];
        const uint64_t group = fnv(&inst.mesh, sizeof(inst.mesh), uint64_t(role) + 1);
        identity.instanceId = trackedId(group, position);
        identity.classification = REMIXAPI_INSTANCE_DYNAMIC_EXT;
      }
      remixapi_InstanceInfo info = {};
      info.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
      info.pNext = (isStatic || isTracked || isAdditive) ? static_cast<void *>(&identity)
                   : inst.replacementHash ? static_cast<void *>(&replacement)
                                          : static_cast<void *>(&blend);
      info.mesh = inst.mesh;
      info.categoryFlags = inst.categories;
      if (role == Role::ViewModel && rasterViewModel)
        info.categoryFlags |= REMIXAPI_INSTANCE_CATEGORY_BIT_VIEW_MODEL;
      else if (role == Role::Sky)
        info.categoryFlags |= REMIXAPI_INSTANCE_CATEGORY_BIT_SKY;
      std::memcpy(info.transform.matrix, inst.transform, sizeof(info.transform.matrix));
      info.doubleSided = !inst.singleSided;
      g_api.DrawInstance(&info);
      ++g_stats.submitted;
    }
    tracked = std::move(nextTracked);
    {
      Timer bspTimer(g_usBsp);
      bsp::submit(materialForHash);
    }
  }
  {
    std::lock_guard<std::mutex> lock(g_lightLock);
    // The engine's own light state (engine_lights), as sampled for this frame.
    const auto engineLights = engine_lights::current();
    uint32_t styled = 0, styledOff = 0, duplicates = 0;
    g_lightDefinitions.clear();
    // Dlights the engine also handed to a model (SetLight): one light.
    if (engineLights && !engineLights->dlights.empty())
      for (auto it = g_frameLights.begin(); it != g_frameLights.end();) {
        bool duplicate = false;
        for (const auto &d : engineLights->dlights)
          if (it->second.type != 2 && std::fabs(it->second.position[0] - d.position[0]) < .1f &&
              std::fabs(it->second.position[1] - d.position[1]) < .1f &&
              std::fabs(it->second.position[2] - d.position[2]) < .1f) {
            duplicate = true;
            break;
          }
        duplicates += duplicate;
        it = duplicate ? g_frameLights.erase(it) : std::next(it);
      }
    auto addDefined = [](uint64_t hash, const SourceLight &l) {
      g_frameLights[hash] = l;
      g_lightDefinitions[hash] = fnv(&l, sizeof(l)) | 1; // never 0 (a light without a definition)
    };
    for (const auto &b : bsp::lights()) {
      SourceLight l = {};
      l.type = b.type;
      std::memcpy(l.color, b.color, 12);
      std::memcpy(l.position, b.position, 12);
      std::memcpy(l.direction, b.direction, 12);
      l.attenuation0 = b.attenuation[0];
      l.attenuation1 = b.attenuation[1];
      l.attenuation2 = b.attenuation[2];
      l.thetaDot = b.thetaDot;
      l.phiDot = b.phiDot;
      if (b.style) {
        // Switchable and flickering lights: scaled by their lightstyle (off at 0).
        const float value = engineLights && engineLights->styles && b.style > 0 && b.style < 128
                                ? engineLights->style[b.style]
                                : 1.0f;
        ++styled;
        if (!(value > 0)) {
          ++styledOff;
          continue;
        }
        for (float &c : l.color)
          c *= value;
        addDefined(b.hash, l);
        continue;
      }
      g_frameLights[b.hash] = l;
    }
    if (engineLights)
      for (const auto *list : {&engineLights->dlights, &engineLights->projected})
        for (const auto &e : *list) {
          SourceLight l = {};
          l.type = e.type;
          std::memcpy(l.color, e.color, 12);
          std::memcpy(l.position, e.position, 12);
          std::memcpy(l.direction, e.direction, 12);
          l.attenuation0 = e.attenuation[0];
          l.attenuation1 = e.attenuation[1];
          l.attenuation2 = e.attenuation[2];
          l.thetaDot = e.thetaDot;
          l.phiDot = e.phiDot;
          addDefined(e.hash, l);
        }
    engine_lights::report(styled, styledOff, duplicates);
    for (auto &[hash, l] : g_frameLights) {
      auto &cached = g_lights[hash];
      const auto defined = g_lightDefinitions.find(hash);
      const uint64_t definition = defined == g_lightDefinitions.end() ? 0 : defined->second;
      if (!cached.handle || cached.definition != definition) {
        // CreateLight under an existing hash replaces that light in place
        // (same handle).
        const remixapi_LightHandle handle = createLight(l, hash);
        if (!handle)
          continue;
        cached.handle = handle;
        cached.definition = definition;
      }
      cached.lastFrame = g_frame;
      g_api.DrawLightInstance(cached.handle);
      ++g_stats.lights;
    }
    if (g_skyDome)
      g_api.DrawLightInstance(g_skyDome); // the captured 2D skybox (setSkyFaces)
    g_lastLights.swap(g_frameLights);
    g_frameLights.clear();
    for (auto it = g_lights.begin(); it != g_lights.end();) {
      if (g_frame - it->second.lastFrame > 300) {
        if (it->second.handle)
          g_api.DestroyLight(it->second.handle);
        it = g_lights.erase(it);
      } else {
        ++it;
      }
    }
  }

  if (g_uiCallback)
    g_uiCallback();
  remixapi_UIDrawList ui = {};
  ui.sType = REMIXAPI_STRUCT_TYPE_UI_DRAW_LIST;
  ui.displayWidth = g_displayWidth;
  ui.displayHeight = g_displayHeight;
  ui.pVertices = g_uiVertices.data();
  ui.vertexCount = uint32_t(g_uiVertices.size());
  ui.pIndices = g_uiIndices.data();
  ui.indexCount = uint32_t(g_uiIndices.size());
  ui.pCommands = g_uiCommands.data();
  ui.commandCount = uint32_t(g_uiCommands.size());
  g_api.SubmitUIDrawList(&ui);
  g_uiVertices.clear();
  g_uiIndices.clear();
  g_uiCommands.clear();

  serviceScreenshotRequest();
  {
    Timer present(g_usPresent);
    g_api.Present(nullptr);
  }
  g_instances.clear();
  g_views.clear();
  g_worldViews.clear();
  g_viewOrder = 0;

  for (auto mesh : g_transient)
    g_api.DestroyMesh(mesh);
  g_stats.transient += g_transient.size();
  g_transient.clear();
  // Static meshes keyed by buffer version: a rewritten buffer never matches
  // again, so old entries age out.
  ++g_frame;
  for (auto it = g_meshes.begin(); it != g_meshes.end();) {
    if (g_frame - it->second.lastFrame > 600) {
      g_api.DestroyMesh(it->second.handle);
      ++g_stats.evicted;
      it = g_meshes.erase(it);
    } else {
      ++it;
    }
  }
}

// Caller holds g_textureLock.
void storeCpuCopy(const void *texture, uint32_t width, uint32_t height, D3DFORMAT format, std::vector<uint8_t> pixels) {
  if (!keepCpuCopy(format, width, height))
    return;
  auto &cpu = g_cpuTextures[texture];
  if (!cpu || cpu->width != width || cpu->height != height || cpu->format != format)
    cpu = std::make_shared<CpuTexture>();
  cpu->width = width;
  cpu->height = height;
  cpu->format = format;
  cpu->data = std::move(pixels);
  cpu->touch(0, 0, width, height);
  ++cpu->version;
}

void onTextureUpload(const void *texture, uint32_t width, uint32_t height, D3DFORMAT format, const uint8_t *data,
                     uint32_t pitch, uint32_t levels) {
  if (!g_ready || !data || !width || !height)
    return;
  remixapi_Format remixFormat;
  uint32_t rowBytes, rows;
  const uint32_t blocksW = std::max(1u, (width + 3) / 4), blocksH = std::max(1u, (height + 3) / 4);
  switch (format) {
  case D3DFMT_A8R8G8B8:
  case D3DFMT_X8R8G8B8:
    remixFormat = REMIXAPI_FORMAT_B8G8R8A8_SRGB; // memory order B, G, R, A
    rowBytes = width * 4;
    rows = height;
    break;
  case D3DFMT_DXT1:
    remixFormat = static_cast<remixapi_Format>(134); // BC1_RGBA_SRGB_EXT
    rowBytes = blocksW * 8;
    rows = blocksH;
    break;
  case D3DFMT_DXT5:
    remixFormat = REMIXAPI_FORMAT_BC3_SRGB;
    rowBytes = blocksW * 16;
    rows = blocksH;
    break;
  default:
    ++g_stats.textureSkipped;
    return;
  }
  std::vector<uint8_t> pixels(size_t(rowBytes) * rows);
  for (uint32_t r = 0; r < rows; ++r)
    std::memcpy(pixels.data() + size_t(r) * rowBytes, data + size_t(r) * pitch, rowBytes);
  // Hashed as the game wrote it, before the X8 channel is made opaque.
  const uint64_t contentHash = dxvk::textureHash(pixels.data(), pixels.size());
  noteReplacementKey(contentHash, 'T');
  if (format == D3DFMT_X8R8G8B8)
    for (size_t i = 3; i < pixels.size(); i += 4)
      pixels[i] = 0xff;

  std::lock_guard<std::mutex> lock(g_textureLock);
  if (auto shared = g_texturesByHash.find(contentHash); shared != g_texturesByHash.end()) {
    if (auto existing = shared->second.lock()) {
      g_textures[texture] = std::move(existing);
      storeCpuCopy(texture, width, height, format, std::move(pixels));
      return;
    }
  }
  auto t = std::make_shared<RemixTexture>();
  t->hash = contentHash ? contentHash : 1;
  remixapi_TextureInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
  info.hash = t->hash;
  info.width = width;
  info.height = height;
  info.depth = 1;
  info.mipLevels = 1;
  info.format = remixFormat;
  info.data = pixels.data();
  info.dataSize = pixels.size();
  // Only level 0 is captured: the runtime builds the chain for textures the
  // game made mipmapped (single-level ones, e.g. lookup gradients, stay so).
  remixapi_TextureInfoGenerateMipsEXT mips = {};
  mips.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_GENERATE_MIPS_EXT;
  mips.generate = true;
  if (levels > 1)
    info.pNext = &mips;
  const bool created = g_api.CreateTexture(&info, &t->texture) == REMIXAPI_ERROR_CODE_SUCCESS;
  ++g_stats.textures;
  if (created) {
    g_texturesByHash[contentHash] = t;
    g_textures[texture] = std::move(t);
  }
  storeCpuCopy(texture, width, height, format, std::move(pixels));
}

void onTexturePartialUpload(const void *texture, uint32_t width, uint32_t height, D3DFORMAT format, const RECT &rect,
                            const uint8_t *data, uint32_t pitch) {
  if (!g_ready || !data || !keepCpuCopy(format, width, height))
    return;
  uint32_t rowBytes, rows;
  if (!levelLayout(format, width, height, rowBytes, rows))
    return;
  const bool blocks = format == D3DFMT_DXT1 || format == D3DFMT_DXT5;
  const uint32_t unit = blocks ? 4 : 1; // pixels per row/column unit
  const uint32_t unitBytes = rowBytes / std::max(1u, blocks ? (width + 3) / 4 : width);
  const uint32_t x0 = uint32_t(rect.left) / unit, y0 = uint32_t(rect.top) / unit;
  const uint32_t x1 = (uint32_t(rect.right) + unit - 1) / unit, y1 = (uint32_t(rect.bottom) + unit - 1) / unit;
  std::lock_guard<std::mutex> lock(g_textureLock);
  auto &cpu = g_cpuTextures[texture];
  if (!cpu || cpu->width != width || cpu->height != height || cpu->format != format) {
    cpu = std::make_shared<CpuTexture>();
    cpu->width = width;
    cpu->height = height;
    cpu->format = format;
    cpu->data.assign(size_t(rowBytes) * rows, 0);
  }
  // GMod rewrites a Chromium page's whole rectangle on every repaint: only
  // the bytes that differ from the copy mark it dirty (the region forwarded
  // to the runtime), unchanged rows are not even copied.
  uint32_t changedX0 = ~0u, changedX1 = 0, changedY0 = ~0u, changedY1 = 0;
  for (uint32_t y = y0; y < std::min(y1, rows); ++y) {
    const uint32_t bytes = (std::min(x1, rowBytes / unitBytes) - x0) * unitBytes;
    uint8_t *to = cpu->data.data() + size_t(y) * rowBytes + size_t(x0) * unitBytes;
    const uint8_t *from = data + size_t(y - y0) * pitch;
    if (std::memcmp(to, from, bytes) == 0)
      continue;
    uint32_t first = 0, last = bytes;
    while (first < bytes && to[first] == from[first])
      ++first;
    while (last > first && to[last - 1] == from[last - 1])
      --last;
    changedX0 = std::min(changedX0, x0 + first / unitBytes);
    changedX1 = std::max(changedX1, x0 + (last + unitBytes - 1) / unitBytes);
    changedY0 = std::min(changedY0, y);
    changedY1 = y + 1;
    std::memcpy(to, from, bytes);
  }
  if (changedX1 <= changedX0)
    return;
  cpu->touch(changedX0 * unit, changedY0 * unit, changedX1 * unit, changedY1 * unit);
  ++cpu->version;
}

void onTextureCopy(const void *source, const void *destination) {
  std::lock_guard<std::mutex> lock(g_textureLock);
  auto it = g_textures.find(source);
  if (it != g_textures.end())
    g_textures[destination] = it->second;
  auto cpu = g_cpuTextures.find(source);
  if (cpu != g_cpuTextures.end())
    g_cpuTextures[destination] = cpu->second;
}

void onTextureReleased(const void *texture) {
  std::lock_guard<std::mutex> lock(g_textureLock);
  g_textures.erase(texture);
  auto cpu = g_cpuTextures.find(texture);
  if (cpu != g_cpuTextures.end()) {
    // UI ids name the CPU copy, which UpdateTexture destinations may share.
    if (cpu->second.use_count() == 1 && cpu->second->registeredVersion && g_ready)
      g_api.FreeUITexture(reinterpret_cast<uintptr_t>(cpu->second.get()));
    g_cpuTextures.erase(cpu);
  }
}

void dumpStatistics(FILE *out) {
  const uint32_t frames = g_stats.frames.exchange(0);
  const double f = frames ? frames : 1;
  fprintf(out,
          "== Remix per frame (%u frames, %zu cached meshes): draws %.1f accepted %.1f submitted %.1f | skipped: "
          "offTarget %.1f noPos %.1f noData %.1f skinned %.1f badPrim %.1f badRange %.1f | hits %.1f created %.1f "
          "transient %.1f evicted %.1f\n",
          frames, g_meshes.size(), g_stats.draws.exchange(0) / f, g_stats.accepted.exchange(0) / f,
          g_stats.submitted.exchange(0) / f, g_stats.offTarget.exchange(0) / f, g_stats.noPosition.exchange(0) / f,
          g_stats.noData.exchange(0) / f, g_stats.skinned.exchange(0) / f, g_stats.badPrim.exchange(0) / f,
          g_stats.badRange.exchange(0) / f, g_stats.cacheHits.exchange(0) / f, g_stats.created.exchange(0) / f,
          g_stats.transient.exchange(0) / f, g_stats.evicted.exchange(0) / f);
  fprintf(out, "   world-list draws: dropped (BSP world) %.1f, kept blended %.1f, kept dynamic %.1f\n",
          g_stats.bspDropped.exchange(0) / f, g_stats.bspKeptBlended.exchange(0) / f,
          g_stats.bspKeptDynamic.exchange(0) / f);
  fprintf(out, "   dropped for the BSP world: static prop draws %.1f, overlay draws %.1f\n",
          g_stats.bspPropsDropped.exchange(0) / f, g_stats.bspOverlaysDropped.exchange(0) / f);
  bsp::dumpStatistics(out);
  remix_ui::dumpStatistics(out);
  {
    std::lock_guard<std::mutex> lock(g_transientSampleLock);
    for (const auto &[hash, n] : g_keptWorldTextures)
      fprintf(out, "     kept dynamic world draws: texture %016llX (%s) %.1f/frame\n", (unsigned long long)hash,
              bsp::materialName(hash).c_str(), n / f);
    g_keptWorldTextures.clear();
  }
  fprintf(out, "   ms/frame: frame %.2f | proxy draws %.2f (mesh build %.2f) present submit %.2f (BSP world %.2f, Present call %.2f)\n",
          g_usFrame.exchange(0) / f / 1000, g_usDraw.exchange(0) / f / 1000, g_usBuild.exchange(0) / f / 1000,
          g_usSubmit.exchange(0) / f / 1000, g_usBsp.exchange(0) / f / 1000, g_usPresent.exchange(0) / f / 1000);
  if (g_ready) {
    static auto framePerformance = reinterpret_cast<PFN_remixapi_GetFramePerformanceEXT>(
        GetProcAddress(GetModuleHandleW(L"remix.dll"), "remixapi_GetFramePerformanceEXT"));
    remixapi_FramePerformanceEXT perf = {};
    perf.structSize = sizeof(perf);
    if (framePerformance && framePerformance(&perf) == REMIXAPI_ERROR_CODE_SUCCESS)
      fprintf(out,
              "   runtime (last frame): present %.2f scene %.2f record %.2f geometry %.2f upload %.2f fence %.2f "
              "acquire %.2f submit %.2f queuePresent %.2f ms; conversions %llu\n",
              perf.presentMilliseconds, perf.sceneMilliseconds, perf.recordMilliseconds, perf.geometryMilliseconds,
              perf.uploadMilliseconds, perf.fenceMilliseconds, perf.acquireMilliseconds, perf.submitMilliseconds,
              perf.queuePresentMilliseconds, static_cast<unsigned long long>(perf.geometryConversions));
    // GPU side: per-pass times, tracing and local-light counts.
    auto stat = [](const char *name) { return GetProcAddress(GetModuleHandleW(L"remix.dll"), name); };
    static auto passTimings = reinterpret_cast<decltype(&remixapi_GetGpuPassTimingsEXT)>(stat("remixapi_GetGpuPassTimingsEXT"));
    static auto tracing = reinterpret_cast<decltype(&remixapi_GetTracingStatisticsEXT)>(stat("remixapi_GetTracingStatisticsEXT"));
    static auto localLights = reinterpret_cast<decltype(&remixapi_GetLocalLightStatisticsEXT)>(stat("remixapi_GetLocalLightStatisticsEXT"));
    static auto breakdown = reinterpret_cast<decltype(&remixapi_GetFrameBreakdownEXT)>(stat("remixapi_GetFrameBreakdownEXT"));
    static remixapi_GpuPassTimingsEXT passes;
    passes = {};
    passes.structSize = sizeof(passes);
    if (passTimings && passTimings(&passes) == REMIXAPI_ERROR_CODE_SUCCESS && passes.available) {
      fprintf(out, "   gpu: frame %.2f ms;", passes.frameMilliseconds);
      for (uint32_t i = 0; i < passes.passCount && i < REMIXAPI_GPU_PASS_TIMING_CAPACITY_EXT; ++i)
        if (passes.passes[i].milliseconds >= 0.5)
          fprintf(out, " %s %.2f", passes.passes[i].name, passes.passes[i].milliseconds);
      fprintf(out, "\n");
    }
    {
      std::lock_guard<std::mutex> guard(g_gpuAverages.lock);
      if (g_gpuAverages.count) {
        const double n = double(g_gpuAverages.count);
        fprintf(out, "   gpu mean (%llu frames): frame %.2f ms;", static_cast<unsigned long long>(g_gpuAverages.count),
                g_gpuAverages.frame / n);
        for (const auto &[name, ms] : g_gpuAverages.passes)
          if (ms / n >= 0.3)
            fprintf(out, " %s %.2f;", name.c_str(), ms / n);
        fprintf(out, "\n");
        g_gpuAverages.passes.clear();
        g_gpuAverages.frame = 0;
        g_gpuAverages.count = 0;
      }
    }
    remixapi_TracingStatisticsEXT trace = {};
    trace.structSize = sizeof(trace);
    remixapi_LocalLightStatisticsEXT lights = {};
    lights.structSize = sizeof(lights);
    remixapi_FrameBreakdownEXT frame = {};
    frame.structSize = sizeof(frame);
    if (tracing && tracing(&trace) == REMIXAPI_ERROR_CODE_SUCCESS && localLights &&
        localLights(&lights) == REMIXAPI_ERROR_CODE_SUCCESS && breakdown && breakdown(&frame) == REMIXAPI_ERROR_CODE_SUCCESS)
      fprintf(out,
              "   trace: instances %llu surfaces %llu | blas builds %llu refits %llu | accel %.0f MiB | local lights %u "
              "(active %u)\n",
              static_cast<unsigned long long>(frame.traceInstances), static_cast<unsigned long long>(frame.traceSurfaces),
              static_cast<unsigned long long>(trace.blasBuilds), static_cast<unsigned long long>(trace.blasRefits),
              trace.accelerationBytes / 1048576.0, lights.lightCount, lights.activeLights);
    if (localLights)
      fprintf(out, "   local light tree: generation %llu, builds %llu, refits %llu, denoiser %u | PVS culling: %u "
                   "clustered, %u unrestricted\n",
              static_cast<unsigned long long>(lights.generation), static_cast<unsigned long long>(lights.builds),
              static_cast<unsigned long long>(lights.refits), lights.denoiserActive, lights.reserved & 0xffff,
              lights.reserved >> 16);
    static auto guiding = reinterpret_cast<decltype(&remixapi_GetLocalLightGuidingStatisticsEXT)>(
        stat("remixapi_GetLocalLightGuidingStatisticsEXT"));
    remixapi_LocalLightGuidingStatisticsEXT gs = {};
    gs.structSize = sizeof(gs);
    if (guiding && guiding(&gs) == REMIXAPI_ERROR_CODE_SUCCESS)
      fprintf(out,
              "   local light samples (1/64 sampled): %u, guided %u, visible %u (%.0f%%) | estimator %s%s | guiding "
              "active %u, history resets %llu / %llu frames %s\n",
              gs.sampledQueries, gs.sampledGuided, gs.sampledVisible,
              gs.sampledQueries ? 100.0 * gs.sampledVisible / gs.sampledQueries : 0.0, gs.reserved & 1 ? "RIS" : "BVH",
              gs.reserved & 2 ? " (sphere-only set)" : "", gs.active, (unsigned long long)gs.historyResets,
              (unsigned long long)gs.renderedFrames, gs.unavailableReason);
    static auto denoising = reinterpret_cast<decltype(&remixapi_GetDenoisingStatisticsEXT)>(stat("remixapi_GetDenoisingStatisticsEXT"));
    remixapi_DenoisingStatisticsEXT dn = {};
    dn.structSize = sizeof(dn);
    if (denoising && denoising(&dn) == REMIXAPI_ERROR_CODE_SUCCESS)
      fprintf(out, "   denoiser: active %u taa %u | history resets %llu, rendered frames %llu, history frames %llu\n",
              dn.activeDenoiser, dn.activeTaa, static_cast<unsigned long long>(dn.historyResets),
              static_cast<unsigned long long>(dn.renderedFrames), static_cast<unsigned long long>(dn.historyFrames));
    static auto sharc = reinterpret_cast<decltype(&remixapi_GetSharcStatisticsEXT)>(stat("remixapi_GetSharcStatisticsEXT"));
    remixapi_SharcStatisticsEXT cache = {};
    cache.structSize = sizeof(cache);
    if (sharc && sharc(&cache) == REMIXAPI_ERROR_CODE_SUCCESS && cache.active)
      fprintf(out, "   sharc: entries %u/%u, update paths %u, queries %u hits %u (%.0f%%), resets %llu of %llu frames\n",
              cache.occupiedEntries, cache.capacity, cache.updatePaths, cache.queryAttempts, cache.queryHits,
              cache.queryAttempts ? 100.0 * cache.queryHits / cache.queryAttempts : 0.0,
              static_cast<unsigned long long>(cache.resets), static_cast<unsigned long long>(cache.renderedFrames));
  }
  fprintf(out, "   transient: %.0f vertices %.0f indices per frame; by declaration:",
          g_transientVerts.exchange(0) / f, g_transientIndices.exchange(0) / f);
  {
    std::lock_guard<std::mutex> lock(g_transientSampleLock);
    for (auto &[decl, n] : g_transientDecls)
      fprintf(out, " %p:%.1f", decl, n / f);
    g_transientDecls.clear();
  }
  fprintf(out, "\n");
  fprintf(out, "   lights per frame %.1f (cached %zu); sky radiance %.3f %.3f %.3f\n", g_stats.lights.exchange(0) / f,
          g_lights.size(), g_lastSky[0], g_lastSky[1], g_lastSky[2]);
  {
    std::lock_guard<std::mutex> lock(g_lightLock);
    for (auto &[h, l] : g_lastLights)
      fprintf(out, "    light type %d color %.3f %.3f %.3f pos %.0f %.0f %.0f dir %.2f %.2f %.2f att %.4g %.4g %.4g\n",
              l.type, l.color[0], l.color[1], l.color[2], l.position[0], l.position[1], l.position[2], l.direction[0],
              l.direction[1], l.direction[2], l.attenuation0, l.attenuation1, l.attenuation2);
  }
  {
    char diagnostic[512] = {};
    auto modsDiagnostic = reinterpret_cast<decltype(&remixapi_GetUSDModsDiagnosticEXT)>(
        GetProcAddress(GetModuleHandleW(L"remix.dll"), "remixapi_GetUSDModsDiagnosticEXT"));
    if (g_ready && modsDiagnostic)
      modsDiagnostic(diagnostic, sizeof(diagnostic));
    fprintf(out, "   USD mods: configure %d; %s\n", int(g_modsResult), diagnostic);
  }
  fprintf(out, "   frames with a sky view %.2f (skybox view had the most draws %.2f); main camera from the engine's world view %.2f\n%s",
          g_stats.skyFrames.exchange(0) / f, g_stats.skyViewDemoted.exchange(0) / f,
          g_stats.worldViewCameras.exchange(0) / f, g_viewSummary.c_str());
  fprintf(out, "   player body (l4d2-rtx shadow mesh) %.1f\n", g_stats.playerBody.exchange(0) / f);
  fprintf(out, "   rtx.conf texture categories: %zu hashes; ignored/hidden draws %.1f\n", categories::size(),
          g_stats.ignored.exchange(0) / f);
  fprintf(out, "   depthOnly %.1f additive %.1f other blended %.1f duplicate %.1f; engine flashlight passes dropped %.1f\n",
          g_stats.depthOnly.exchange(0) / f, g_stats.additive.exchange(0) / f, g_stats.blended.exchange(0) / f,
          g_stats.duplicate.exchange(0) / f, g_stats.flashlightPasses.exchange(0) / f);
  fprintf(out, "   UI draws %.1f (render-target sampling skipped %.1f); CPU texture copies %zu\n",
          g_stats.uiDraws.exchange(0) / f, g_stats.uiNoTexture.exchange(0) / f, [] {
            std::lock_guard<std::mutex> lock(g_textureLock);
            return g_cpuTextures.size();
          }());
  {
    std::lock_guard<std::mutex> lock(g_textureLock);
    size_t bytes = 0;
    for (auto &[t, c] : g_cpuTextures)
      bytes += c->data.size();
    fprintf(out, "   CPU texture copies: %.1f MiB; UI texture registrations per frame %.2f, region updates %.2f (%.2f MiB, %.2f ms, runtime %.2f ms)\n",
            bytes / 1048576.0, g_uiRegistrations.exchange(0) / f, g_uiRegionUpdates.exchange(0) / f,
            g_uiRegisteredBytes.exchange(0) / f / 1048576.0,
            g_usUiRegister.exchange(0) / f / 1000, g_usUiRegisterApi.exchange(0) / f / 1000);
  }
  fprintf(out, "   flat (2D) %.1f; textures uploaded %u, unsupported formats %u, live %zu\n",
          g_stats.flat.exchange(0) / f, g_stats.textures.exchange(0), g_stats.textureSkipped.exchange(0), [] {
            std::lock_guard<std::mutex> lock(g_textureLock);
            return g_textures.size();
          }());
  fprintf(out, "   nonFinite %u rejected %u; camera (%u draws) view row3 %.1f %.1f %.1f proj diag %.3f %.3f %.4f %.3f\n",
          g_stats.nonFinite.exchange(0), g_stats.rejected.exchange(0), g_lastCamera.draws, g_lastCamera.view[12],
          g_lastCamera.view[13], g_lastCamera.view[14], g_lastCamera.projection[0], g_lastCamera.projection[5],
          g_lastCamera.projection[10], g_lastCamera.projection[14]);
}

} // namespace proxy::remix

// Exports for in-process UI contributors (see g_uiCallback).
extern "C" __declspec(dllexport) void __cdecl openremix_ui_set_callback(void(__cdecl *callback)()) {
  proxy::remix::g_uiCallback = callback;
}

// Adds draws to this frame's UI list. Command index offsets refer to
// `indices`; indices refer to `vertices` (positions in back-buffer pixels).
extern "C" __declspec(dllexport) void __cdecl openremix_ui_append(const remixapi_UIVertex *vertices,
                                                                   uint32_t vertexCount, const uint32_t *indices,
                                                                   uint32_t indexCount,
                                                                   const remixapi_UIDrawCommand *commands,
                                                                   uint32_t commandCount) {
  using namespace proxy::remix;
  if (!vertices || !indices || !commands)
    return;
  const uint32_t baseVertex = uint32_t(g_uiVertices.size()), baseIndex = uint32_t(g_uiIndices.size());
  g_uiVertices.insert(g_uiVertices.end(), vertices, vertices + vertexCount);
  for (uint32_t i = 0; i < indexCount; ++i)
    g_uiIndices.push_back(baseVertex + indices[i]);
  for (uint32_t c = 0; c < commandCount; ++c) {
    remixapi_UIDrawCommand command = commands[c];
    command.indexOffset += baseIndex;
    g_uiCommands.push_back(command);
  }
}

// dxvk-remix variables that map onto proxy behaviour. Returns 1 when handled;
// callers forward other variables to the runtime.
extern "C" __declspec(dllexport) int __cdecl openremix_set_legacy_variable(const char *key, const char *value) {
  if (!key || !value)
    return 0;
  if (proxy::categories::set(key, value))
    return 1;
  if (std::strcmp(key, "rtx.skyBrightness") == 0) {
    const float brightness = float(atof(value));
    proxy::remix::g_skyBrightness = std::isfinite(brightness) && brightness > 0 ? brightness : 1.0f;
    return 1;
  }
  return 0;
}

namespace proxy::remix {
bool onMarker(const char *text) {
  constexpr char kPrefix[] = "openremix:";
  if (!text || std::strncmp(text, kPrefix, sizeof(kPrefix) - 1) != 0)
    return false;
  text += sizeof(kPrefix) - 1;
  auto argument = [&](const char *name) -> const char * {
    const size_t n = std::strlen(name);
    if (std::strncmp(text, name, n) != 0 || (text[n] != ' ' && text[n] != 0))
      return nullptr;
    return text[n] == ' ' ? text + n + 1 : text + n;
  };
  auto depth = [](int &counter, const char *value) {
    counter += std::atoi(value) ? 1 : -1;
    if (counter < 0)
      counter = 0;
  };
  if (const char *v = argument("world_lists")) {
    if (g_worldLists == 0 && std::atoi(v) && g_shaderApi && g_worldViews.size() < 16 && d3d::backBufferBound()) {
      WorldView w;
      getMatrix(kMatrixView, w.view);
      getMatrix(kMatrixProjection, w.projection);
      // As onDraw stores views: left-handed.
      if (std::fabs(std::fabs(w.projection[11]) - 1.0f) < 1e-3f && std::fabs(w.projection[15]) < 1e-3f) {
        if (w.projection[11] < 0) {
          for (int r = 0; r < 4; ++r)
            w.view[r * 4 + 2] = -w.view[r * 4 + 2];
          for (int c = 0; c < 4; ++c)
            w.projection[8 + c] = -w.projection[8 + c];
        }
        g_worldViews.push_back(w);
      }
    }
    depth(g_worldLists, v);
  }
  else if (const char *v = argument("overlays"))
    depth(g_overlays, v);
  else if (const char *v = argument("body"))
    g_markerCategories = std::atoi(v) ? REMIXAPI_INSTANCE_CATEGORY_BIT_THIRD_PERSON_PLAYER_MODEL |
                                            REMIXAPI_INSTANCE_CATEGORY_BIT_THIRD_PERSON_PLAYER_BODY
                                      : 0;
  else if (const char *v = argument("lights"))
    engine_lights::activate(uint32_t(std::strtoul(v, nullptr, 10)));
  else if (const char *v = argument("map")) {
    if (g_ready) {
      resetSky(); // the previous map's dome; this map's sky draws follow
      proxy::bsp::load(g_api, v);
    }
  } else
    return false;
  return true;
}
} // namespace proxy::remix

// l4d2-rtx: the map that is loading (nullptr or "" when leaving it). Returns
// 1 if its world is taken from the BSP, so the engine's world draws are dropped.
extern "C" __declspec(dllexport) int __cdecl openremix_proxy_load_map(const char *mapName) {
  using namespace proxy::remix;
  if (!g_ready)
    return 0;
  return proxy::bsp::load(g_api, mapName ? mapName : "") ? 1 : 0;
}

// l4d2-rtx: whether the BSP world draws the static prop at this placement
// (matrix3x4_t, model to world); the engine can skip drawing it. Thread-safe.
extern "C" __declspec(dllexport) int __cdecl openremix_proxy_provides_prop(const float *modelToWorld) {
  return proxy::bsp::providesProp(modelToWorld) ? 1 : 0;
}

// l4d2-rtx: wraps IVRenderView::DrawWorldLists (1 before, 0 after).
extern "C" __declspec(dllexport) void __cdecl openremix_proxy_world_lists(int begin) {
  using namespace proxy::remix;
  g_worldLists += begin ? 1 : -1;
  if (g_worldLists < 0)
    g_worldLists = 0;
}

// l4d2-rtx: wraps COverlayMgr::RenderOverlays (1 before, 0 after).
extern "C" __declspec(dllexport) void __cdecl openremix_proxy_overlays(int begin) {
  using namespace proxy::remix;
  g_overlays += begin ? 1 : -1;
  if (g_overlays < 0)
    g_overlays = 0;
}
