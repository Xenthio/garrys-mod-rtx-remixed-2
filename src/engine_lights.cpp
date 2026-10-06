#include "engine_lights.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace proxy::engine {
// engine_hooks.cpp: a marker through the render context, ordered with the
// frame's draws when the material system queues rendering.
void marker(const char *text);
} // namespace proxy::engine

namespace proxy::engine_lights {
namespace {

// Everything below is for one engine build: each interface must have its
// vtable, and each slot used its function, at these engine.dll RVAs (checked
// in install(); a source that does not match stays off).
//   CEngineClient (VEngineClient013): 44 LightStyleValue(int) -> float,
//     51 GetLevelName() -> const char *
//   CVEfx (VEngineEffects001): 6 int CL_GetActiveDLights(dlight_t *list[32])
//   CShadowMgr (VEngineShadowMgr002): 1 DestroyShadow(ShadowHandle_t),
//     16 UpdateFlashlightState(ShadowHandle_t, const FlashlightState_t &)
constexpr uintptr_t kClientVtable = 0x3a3078, kLightStyleValue = 0x7f770, kGetLevelName = 0x7ede0;
constexpr uintptr_t kEffectsVtable = 0x3cd0f8, kGetActiveDLights = 0x143320;
constexpr uintptr_t kShadowMgrVtable = 0x3cd330, kDestroyShadow = 0x149d40, kUpdateFlashlightState = 0x1506a0;
constexpr unsigned kSlotLightStyleValue = 44, kSlotGetLevelName = 51, kSlotGetActiveDLights = 6, kSlotDestroyShadow = 1,
                   kSlotUpdateFlashlightState = 16;
constexpr int kMaxDLights = 32;

// dlight_t (public/dlight.h), 0x48 bytes on x64.
struct DLight {
  int32_t flags; // 1 no world illumination, 2 no model illumination
  float origin[3], radius;
  uint8_t color[3];
  int8_t exponent;
  double die;
  float decay, minlight;
  int32_t key, style;
  float direction[3], innerAngle, outerAngle;
};
static_assert(sizeof(DLight) == 0x48 && offsetof(DLight, die) == 0x18 && offsetof(DLight, direction) == 0x30);

// FlashlightState_t: the part every build shares (the struct is 0x98 bytes
// here, 0x88 in the SDK headers); only this much is ever copied.
struct FlashlightState {
  float origin[3], orientation[4]; // quaternion x, y, z, w
  float nearZ, farZ, horizontalFov, verticalFov;
  float quadratic, linear, constant;
  float color[4];
  void *spotlightTexture;
  int32_t spotlightFrame;
};
constexpr size_t kFlashlightStateSize = 0x54;
static_assert(offsetof(FlashlightState, color) == 0x38 && offsetof(FlashlightState, spotlightTexture) == 0x48 &&
              offsetof(FlashlightState, spotlightFrame) + 4 == kFlashlightStateSize);

uintptr_t g_engine = 0, g_engineEnd = 0;
void *g_client = nullptr, *g_effects = nullptr, *g_shadowMgr = nullptr;
std::atomic<bool> g_installed{false}, g_styles{false}, g_level{false}, g_dlights{false}, g_projected{false};
std::atomic<const char *> g_stylesStatus{"not installed"}, g_dlightsStatus{"not installed"},
    g_projectedStatus{"not installed"};

// Projected textures by engine shadow handle, as the client last pushed them.
// sample: g_samples when it was pushed (see the level change in sample()).
struct Projected {
  FlashlightState state;
  uint64_t sample;
};
std::mutex g_projectedLock;
std::unordered_map<uint16_t, Projected> g_projectedStates;
std::atomic<uint64_t> g_samples{0};
std::atomic<void *> g_updateOriginal{nullptr}, g_destroyOriginal{nullptr};

// Main thread to render thread: snapshots waiting for their marker.
std::mutex g_snapshotLock;
std::deque<std::pair<uint32_t, std::shared_ptr<const Snapshot>>> g_pending;
std::shared_ptr<const Snapshot> g_current;
std::shared_ptr<const Snapshot> g_published; // main thread
uint32_t g_sequence = 0;                     // main thread
char g_levelName[256] = {};                  // main thread

struct Statistics {
  std::atomic<uint32_t> samples{0}, published{0}, updates{0}, destroys{0}, invalidProjected{0}, staleProjected{0};
  std::atomic<uint32_t> frames{0}, styled{0}, styledOff{0}, duplicates{0};
} g_stats;

void *slot(void *object, unsigned index) { return (*static_cast<void ***>(object))[index]; }
bool vtableAt(void *object, uintptr_t rva) { return *static_cast<uintptr_t *>(object) == g_engine + rva; }
bool slotAt(void *object, unsigned index, uintptr_t rva) {
  return reinterpret_cast<uintptr_t>(slot(object, index)) == g_engine + rva;
}

float envScale(const char *name, float fallback) {
  char v[32] = {};
  const float value = GetEnvironmentVariableA(name, v, sizeof(v)) ? float(atof(v)) : 0;
  return value > 0 ? value : fallback;
}

// IShadowMgr hooks (main thread: the client's Simulate, Lua). ShadowHandle_t
// is an unsigned short in DX. The return value is passed through untouched
// (both are void; their error paths tail-call a message function).
uintptr_t updateFlashlightState(void *self, uint16_t handle, const void *state) {
  if (state && handle != 0xffff) {
    Projected p;
    std::memcpy(&p.state, state, kFlashlightStateSize);
    p.sample = g_samples.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_projectedLock);
    g_projectedStates[handle] = p;
  }
  ++g_stats.updates;
  using Fn = uintptr_t (*)(void *, uint16_t, const void *);
  return reinterpret_cast<Fn>(g_updateOriginal.load(std::memory_order_relaxed))(self, handle, state);
}
uintptr_t destroyShadow(void *self, uint16_t handle) {
  {
    std::lock_guard<std::mutex> lock(g_projectedLock);
    g_stats.destroys += uint32_t(g_projectedStates.erase(handle));
  }
  using Fn = uintptr_t (*)(void *, uint16_t);
  return reinterpret_cast<Fn>(g_destroyOriginal.load(std::memory_order_relaxed))(self, handle);
}

// One pointer write: safe while another thread calls through the vtable.
// The original is stored first, so the hook never runs without it.
bool hookSlot(void *object, unsigned index, void *hook, std::atomic<void *> &original) {
  void **entry = &(*static_cast<void ***>(object))[index];
  original = *entry;
  DWORD old;
  if (!VirtualProtect(entry, sizeof(void *), PAGE_READWRITE, &old))
    return false;
  InterlockedExchangePointer(entry, hook);
  VirtualProtect(entry, sizeof(void *), old, &old);
  return true;
}

bool finite3(const float *v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

// As the engine hands a dlight to models (WorldLightFromDynamicLight, engine
// 0x1e37a0): colour = byte * 2^exponent / 255, quadratic falloff only,
// 1 / (max(minlight, 1) * max(radius, 0.1)^2); a spot when m_OuterAngle > 0.
// The engine also cuts the light off at its radius; the path tracer does not.
bool fromDLight(const DLight &d, Light &l) {
  if ((d.flags & 3) == 3 || !(d.radius > 0) || !std::isfinite(d.radius) || !finite3(d.origin))
    return false; // lights neither the world nor models
  if (!(d.color[0] | d.color[1] | d.color[2]))
    return false;
  const float scale = std::ldexp(1.0f / 255.0f, std::clamp<int>(d.exponent, -16, 16));
  for (int i = 0; i < 3; ++i)
    l.color[i] = d.color[i] * scale;
  std::memcpy(l.position, d.origin, 12);
  const float radius = std::max(d.radius, 0.1f), minlight = std::max(std::isfinite(d.minlight) ? d.minlight : 1, 1.0f);
  l.attenuation[2] = 1 / (minlight * radius * radius);
  l.type = 1;
  if (d.outerAngle > 0 && d.outerAngle < 180 && std::isfinite(d.innerAngle) && finite3(d.direction)) {
    l.type = 3;
    std::memcpy(l.direction, d.direction, 12);
    l.thetaDot = std::cos(d.innerAngle * 0.01745329252f);
    l.phiDot = std::cos(d.outerAngle * 0.01745329252f);
  }
  return true;
}

// A projected texture as a spot light. Source's flashlight shaders light a
// surface with
//   scale * m_Color * cookie * saturate(constant + linear / d + quadratic / d^2) * N.L
// (SetFlashLightColorFromState: scale 0.25 with HDR), fading out over the
// last 40% of m_FarZ. The path tracer has inverse-square lights only: the
// irradiance is matched at a reference distance (a quarter of m_FarZ, 64 to
// 256 units; OPENREMIX_PROXY_PROJECTED_REFERENCE) and falls off by 1/d^2
// around it. Approximations: the frustum becomes its inscribed cone (half
// the larger FOV) with the outer half fading, in place of the cookie texture
// (ignored); nothing ends the light at m_FarZ. Shadows are the path
// tracer's. OPENREMIX_PROXY_PROJECTED_SCALE scales the brightness.
bool fromProjected(const FlashlightState &s, Light &l) {
  static const float brightness = 0.25f * envScale("OPENREMIX_PROXY_PROJECTED_SCALE", 1.0f);
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
    return false; // dark: nothing to submit
  l.type = 3;
  std::memcpy(l.position, s.origin, 12);
  // Forward: +X of the rotated basis.
  l.direction[0] = 1 - 2 * (q[1] * q[1] + q[2] * q[2]);
  l.direction[1] = 2 * (q[0] * q[1] + q[3] * q[2]);
  l.direction[2] = 2 * (q[0] * q[2] - q[3] * q[1]);
  l.attenuation[2] = 1 / (reference * reference);
  const float outer = std::clamp(fov * 0.5f, 1.0f, 89.0f) * 0.01745329252f;
  l.phiDot = std::cos(outer);
  l.thetaDot = std::cos(outer * 0.5f);
  return true;
}

bool sameLights(const std::vector<Light> &a, const std::vector<Light> &b) {
  if (a.size() != b.size())
    return false;
  constexpr size_t from = offsetof(Light, color), size = offsetof(Light, phiDot) + sizeof(float) - from;
  for (size_t i = 0; i < a.size(); ++i)
    if (a[i].hash != b[i].hash || a[i].type != b[i].type ||
        std::memcmp(reinterpret_cast<const char *>(&a[i]) + from, reinterpret_cast<const char *>(&b[i]) + from, size))
      return false;
  return true;
}

// Entries a destroy was missed for must not outlive their map. Lua projected
// textures are pushed only when they change, so age says nothing; but the
// previous map's were all pushed before its last sample(), and this map's
// after it.
void noteLevel() {
  using Fn = const char *(*)(void *);
  const char *level = reinterpret_cast<Fn>(slot(g_client, kSlotGetLevelName))(g_client);
  if (!level || std::strncmp(level, g_levelName, sizeof(g_levelName) - 1) == 0)
    return;
  std::snprintf(g_levelName, sizeof(g_levelName), "%s", level);
  const uint64_t now = g_samples.load(std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(g_projectedLock);
  for (auto it = g_projectedStates.begin(); it != g_projectedStates.end();) {
    if (it->second.sample < now) {
      it = g_projectedStates.erase(it);
      ++g_stats.staleProjected;
    } else {
      ++it;
    }
  }
}

} // namespace

bool install() {
  static std::atomic<bool> done{false};
  if (done.exchange(true))
    return g_installed;
  char v[8] = {};
  if (GetEnvironmentVariableA("OPENREMIX_PROXY_ENGINE_LIGHTS", v, sizeof(v)) && v[0] == '0') {
    g_stylesStatus = g_dlightsStatus = g_projectedStatus = "disabled (OPENREMIX_PROXY_ENGINE_LIGHTS=0)";
    return false;
  }
  HMODULE engine = GetModuleHandleA("engine.dll");
  using CreateInterfaceFn = void *(*)(const char *name, int *returnCode);
  auto factory = engine ? reinterpret_cast<CreateInterfaceFn>(
                              reinterpret_cast<void *>(GetProcAddress(engine, "CreateInterface")))
                        : nullptr;
  if (!factory) {
    g_stylesStatus = g_dlightsStatus = g_projectedStatus = "no engine.dll";
    return false;
  }
  g_engine = reinterpret_cast<uintptr_t>(engine);
  const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(engine);
  const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(g_engine + dos->e_lfanew);
  g_engineEnd = g_engine + nt->OptionalHeader.SizeOfImage;

  g_client = factory("VEngineClient013", nullptr);
  if (!g_client)
    g_stylesStatus = "no interface";
  else if (!vtableAt(g_client, kClientVtable))
    g_stylesStatus = "vtable mismatch";
  else if (!slotAt(g_client, kSlotLightStyleValue, kLightStyleValue))
    g_stylesStatus = "slot mismatch";
  else {
    g_stylesStatus = "ok";
    g_level = slotAt(g_client, kSlotGetLevelName, kGetLevelName);
    g_styles = true;
  }

  g_effects = factory("VEngineEffects001", nullptr);
  if (!g_effects)
    g_dlightsStatus = "no interface";
  else if (!vtableAt(g_effects, kEffectsVtable))
    g_dlightsStatus = "vtable mismatch";
  else if (!slotAt(g_effects, kSlotGetActiveDLights, kGetActiveDLights))
    g_dlightsStatus = "slot mismatch";
  else {
    g_dlightsStatus = "ok";
    g_dlights = true;
  }

  // The slots must still hold the engine's own functions: anything else
  // (another hook) is not what was verified.
  g_shadowMgr = factory("VEngineShadowMgr002", nullptr);
  if (!g_shadowMgr)
    g_projectedStatus = "no interface";
  else if (!vtableAt(g_shadowMgr, kShadowMgrVtable))
    g_projectedStatus = "vtable mismatch";
  else if (!slotAt(g_shadowMgr, kSlotDestroyShadow, kDestroyShadow) ||
           !slotAt(g_shadowMgr, kSlotUpdateFlashlightState, kUpdateFlashlightState))
    g_projectedStatus = "slot mismatch";
  // Destroy first: an update is never recorded without its destroy.
  else if (!hookSlot(g_shadowMgr, kSlotDestroyShadow, reinterpret_cast<void *>(&destroyShadow), g_destroyOriginal) ||
           !hookSlot(g_shadowMgr, kSlotUpdateFlashlightState, reinterpret_cast<void *>(&updateFlashlightState),
                     g_updateOriginal))
    g_projectedStatus = "hook failed";
  else {
    g_projectedStatus = "ok (hooked)";
    g_projected = true;
  }
  g_installed = g_styles || g_dlights || g_projected;
  return g_installed;
}

bool stylesActive() { return g_styles; }

// Cheap and idempotent (128 + 1 trivial engine calls, a few dozen lights):
// it runs for every view, nested ones included, and announces a snapshot
// only when it differs from the last.
void sample() {
  if (!g_installed)
    return;
  ++g_stats.samples;
  auto snapshot = std::make_shared<Snapshot>();
  if (g_styles) {
    using Fn = float (*)(void *, int);
    const Fn value = reinterpret_cast<Fn>(slot(g_client, kSlotLightStyleValue));
    for (int i = 0; i < 128; ++i) {
      const float v = value(g_client, i);
      snapshot->style[i] = std::isfinite(v) && v >= 0 ? v : 1.0f;
    }
    snapshot->styles = true;
  }
  // The dlight array belongs to the main thread (CL_DecayLights).
  if (g_dlights) {
    using Fn = int (*)(void *, DLight **);
    DLight *list[kMaxDLights] = {};
    const int count = reinterpret_cast<Fn>(slot(g_effects, kSlotGetActiveDLights))(g_effects, list);
    bool valid = count >= 0 && count <= kMaxDLights;
    for (int i = 0; valid && i < count; ++i) {
      const uintptr_t p = reinterpret_cast<uintptr_t>(list[i]);
      valid = p >= g_engine && p + sizeof(DLight) <= g_engineEnd && p % 4 == 0; // cl_dlights is engine data
    }
    if (!valid) {
      g_dlights = false;
      g_dlightsStatus = "unexpected list (disabled)";
    } else {
      for (int i = 0; i < count; ++i) {
        Light l;
        // The array slot is the identity: keys are 0 for anonymous lights.
        l.hash = 0x646c696768740000ull ^ (reinterpret_cast<uintptr_t>(list[i]) * 0x9e3779b97f4a7c15ull);
        if (fromDLight(*list[i], l))
          snapshot->dlights.push_back(l);
      }
    }
  }
  if (g_projected) {
    if (g_level)
      noteLevel();
    std::lock_guard<std::mutex> lock(g_projectedLock);
    for (const auto &[handle, p] : g_projectedStates) {
      Light l;
      l.hash = 0x70726f6a74657800ull ^ (uint64_t(handle) * 0x9e3779b97f4a7c15ull);
      if (fromProjected(p.state, l))
        snapshot->projected.push_back(l);
      else
        ++g_stats.invalidProjected;
    }
    // unordered_map order is not stable across inserts.
    std::sort(snapshot->projected.begin(), snapshot->projected.end(),
              [](const Light &a, const Light &b) { return a.hash < b.hash; });
  }
  ++g_samples;
  if (g_published && g_published->styles == snapshot->styles &&
      std::memcmp(g_published->style, snapshot->style, sizeof(snapshot->style)) == 0 &&
      sameLights(g_published->dlights, snapshot->dlights) && sameLights(g_published->projected, snapshot->projected))
    return;
  g_published = snapshot;
  ++g_stats.published;
  const uint32_t sequence = ++g_sequence;
  {
    std::lock_guard<std::mutex> lock(g_snapshotLock);
    g_pending.emplace_back(sequence, std::move(snapshot));
    if (g_pending.size() > 64) // markers not arriving: the newest still win
      g_pending.pop_front();
  }
  char text[48];
  std::snprintf(text, sizeof(text), "openremix:lights %u", sequence);
  engine::marker(text);
}

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

void report(uint32_t styled, uint32_t styledOff, uint32_t duplicates) {
  ++g_stats.frames;
  g_stats.styled += styled;
  g_stats.styledOff += styledOff;
  g_stats.duplicates += duplicates;
}

void dumpStatistics(FILE *out) {
  size_t dlights = 0, projected = 0, pending = 0, tracked = 0;
  {
    std::lock_guard<std::mutex> lock(g_snapshotLock);
    if (g_current) {
      dlights = g_current->dlights.size();
      projected = g_current->projected.size();
    }
    pending = g_pending.size();
  }
  {
    std::lock_guard<std::mutex> lock(g_projectedLock);
    tracked = g_projectedStates.size();
  }
  const float frames = float(std::max<uint32_t>(g_stats.frames.exchange(0), 1));
  fprintf(out, "engine lights: styles %s; dlights %s; projected textures %s\n", g_stylesStatus.load(),
          g_dlightsStatus.load(), g_projectedStatus.load());
  fprintf(out,
          "   per frame: styled map lights %.1f (off %.1f), SetLight copies of dlights dropped %.2f; now: dlights %zu, "
          "projected textures %zu (tracked %zu)\n",
          g_stats.styled.exchange(0) / frames, g_stats.styledOff.exchange(0) / frames,
          g_stats.duplicates.exchange(0) / frames, dlights, projected, tracked);
  fprintf(out,
          "   samples %u, snapshots published %u (pending %zu); flashlight updates %u, destroys %u, invalid states %u, "
          "dropped at map change %u\n",
          g_stats.samples.exchange(0), g_stats.published.exchange(0), pending, g_stats.updates.exchange(0),
          g_stats.destroys.exchange(0), g_stats.invalidProjected.exchange(0), g_stats.staleProjected.load());
}

} // namespace proxy::engine_lights
