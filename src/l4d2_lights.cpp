// Left 4 Dead 2: the survivors' flashlights as Remix lights (l4d2-rtx
// main_module.cpp iterate_entities, remix_api.cpp flashlight_frame, the
// game_settings defaults). Source's own projected flashlight is off
// (r_flashlightrender 0, x86_engine's cvars).
//
// Each lit flashlight is two sphere lights with light shaping at the eye,
// moved by an offset along the view axes: a wide outer cone and a narrow
// inner one. The local player's state is read where the client keeps it
// (C_TerrorPlayer: flashlight on, eye position and view axes, offsets of the
// build this was written for); other players and survivor bots by their
// networked variables (m_fEffects EF_DIMLIGHT, m_vecOrigin +
// m_vecViewOffset, m_angEyeAngles), found in the client's receive tables.
//
// Main thread (sampleLights, once per frame): the players' state. Render
// thread (submitLights, while the frame's lights are drawn): the Remix lights.
// OPENREMIX_PROXY_FLASHLIGHTS=0 turns them off; OPENREMIX_PROXY_FLASHLIGHT_SCALE
// scales their intensity.
// The library loader (remixapi_lib_*) is remix_output's.
#define REMIX_WINAPI_NO_LIBRARY_LOADER
#include <remix/remix_c.h>

#include "l4d2.h"

#include "diag.h"
#include "game.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace proxy::l4d2 {
namespace {

std::atomic<const char *> g_status{"not installed"};

void *g_entityList = nullptr;   // VClientEntityList003: slot 3 GetClientEntity(int)
void *g_engineClient = nullptr; // VEngineClient013: slot 12 GetLocalPlayer, slot 21 GetMaxClients
const void *g_terrorPlayerClass = nullptr, *g_survivorBotClass = nullptr; // ClientClass

// C_TerrorPlayer, the local player (l4d2-rtx; not networked).
constexpr int kLocalOn = 0x14D8, kLocalEye = 0x1110, kLocalForward = 0x111C, kLocalUp = 0x1128, kLocalRight = 0x1134;
// Networked (receive tables), l4d2-rtx's values as the fallback.
int g_effects = 0xE0, g_origin = -1, g_viewOffset = -1, g_eyeAngles = 0x196C;

// One flashlight's light (l4d2-rtx game_settings defaults).
struct Cone {
  float radius, angle, softness, exponent, intensity, volumetric;
};
constexpr Cone kOuter{0.16f, 26.0f, 0.3f, 0.8f, 20000.0f, 0.0f}, kInner{0.44f, 17.0f, 0.06f, 0.8f, 5000.0f, 0.0f};
// Along forward, vertical (up), horizontal (right).
constexpr float kPlayerOffset[3] = {-1.5f, -3.9f, -4.8f}, kBotOffset[3] = {22.0f, 1.0f, -4.0f};
float g_scale = 1;

struct Flashlight {
  int index;
  float position[3], forward[3];
};
std::mutex g_lock;
std::vector<Flashlight> g_sampled; // under g_lock

struct Light {
  remixapi_LightHandle handle = nullptr;
  uint64_t definition = 0;
  bool seen = false;
};
std::map<int, Light> g_lights; // render thread; key: entity index * 2 + inner

struct Statistics {
  std::atomic<uint64_t> samples{0}, lit{0}, created{0}, updated{0}, drawn{0}, destroyed{0}, failed{0};
} g_stats;

template <typename T> T read(const void *p, int offset) {
  T value;
  std::memcpy(&value, static_cast<const uint8_t *>(p) + offset, sizeof(T));
  return value;
}

uint64_t fnv(const void *data, size_t n, uint64_t h = 0xcbf29ce484222325ull) {
  for (size_t i = 0; i < n; ++i)
    h = (h ^ static_cast<const uint8_t *>(data)[i]) * 0x100000001b3ull;
  return h;
}

// The receive tables' offsets (pe_module.h).
int findProp(const uint8_t *table, const char *name, int, int) { return x86::findNetProp(table, name); }

bool finite3(const float *v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

bool normalize(float *v) {
  const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (!(length > 1e-3f) || !std::isfinite(length))
    return false;
  for (int i = 0; i < 3; ++i)
    v[i] /= length;
  return true;
}

// Source's AngleVectors (pitch, yaw, roll in degrees).
void angleVectors(const float *angles, float *forward, float *right, float *up) {
  constexpr float kRad = 3.14159265f / 180.0f;
  const float sp = std::sin(angles[0] * kRad), cp = std::cos(angles[0] * kRad);
  const float sy = std::sin(angles[1] * kRad), cy = std::cos(angles[1] * kRad);
  const float sr = std::sin(angles[2] * kRad), cr = std::cos(angles[2] * kRad);
  forward[0] = cp * cy, forward[1] = cp * sy, forward[2] = -sp;
  right[0] = -sr * sp * cy + cr * sy, right[1] = -sr * sp * sy - cr * cy, right[2] = -sr * cp;
  up[0] = cr * sp * cy + sr * sy, up[1] = cr * sp * sy - sr * cy, up[2] = cr * cp;
}

} // namespace

bool installLights() {
  if (!envFlag("FLASHLIGHTS", true)) {
    g_status = "off (OPENREMIX_PROXY_FLASHLIGHTS=0)";
    return false;
  }
  g_scale = envFloat("FLASHLIGHT_SCALE", 1.0f);
  Module client, engine;
  g_entityList = createInterface("client.dll", "VClientEntityList003");
  void *clientDll = createInterface("client.dll", "VClient016");
  g_engineClient = createInterface("engine.dll", "VEngineClient013");
  if (!client.open("client.dll") || !engine.open("engine.dll") || !g_entityList || !clientDll || !g_engineClient ||
      !client.inCode((*static_cast<void ***>(g_entityList))[3]) || !client.inCode((*static_cast<void ***>(clientDll))[7]) ||
      !engine.inCode((*static_cast<void ***>(g_engineClient))[12]) ||
      !engine.inCode((*static_cast<void ***>(g_engineClient))[21])) {
    g_status = "interfaces not found";
    g_entityList = g_engineClient = nullptr;
    diag::stage("L4D2: flashlights: %s", g_status.load());
    return false;
  }
  // IBaseClientDLL slot 7: GetAllClasses. ClientClass {create, create event,
  // network name +8, recv table +0x0C, next +0x10, class id +0x14}.
  using GetAllClassesFn = const uint8_t *(PROXY_THISCALL *)(void *);
  for (const uint8_t *c = reinterpret_cast<GetAllClassesFn>((*static_cast<void ***>(clientDll))[7])(clientDll); c;
       c = read<const uint8_t *>(c, 0x10)) {
    const char *name = read<const char *>(c, 8);
    if (!name)
      continue;
    if (std::strcmp(name, "CTerrorPlayer") == 0)
      g_terrorPlayerClass = c;
    else if (std::strcmp(name, "SurvivorBot") == 0)
      g_survivorBotClass = c;
  }
  if (!g_terrorPlayerClass || !g_survivorBotClass) {
    g_status = "CTerrorPlayer or SurvivorBot client class not found";
    g_entityList = nullptr;
    diag::stage("L4D2: flashlights: %s", g_status.load());
    return false;
  }
  const uint8_t *table = read<const uint8_t *>(g_survivorBotClass, 0x0C);
  const int effects = findProp(table, "m_fEffects", 0, 0), angles = findProp(table, "m_angEyeAngles[0]", 0, 0);
  g_origin = findProp(table, "m_vecOrigin", 0, 0);
  g_viewOffset = findProp(table, "m_vecViewOffset[0]", 0, 0);
  diag::stage("L4D2: flashlights: SurvivorBot m_fEffects +0x%X (expected 0xE0), m_angEyeAngles +0x%X (expected "
              "0x196C), m_vecOrigin +0x%X, m_vecViewOffset +0x%X",
              effects, angles, g_origin, g_viewOffset);
  if (effects > 0)
    g_effects = effects;
  if (angles > 0)
    g_eyeAngles = angles;
  g_status = g_origin > 0 && g_viewOffset > 0 ? "ok" : "ok (other players' and bots' eye position not found)";
  return true;
}

void sampleLights() {
  if (!g_entityList)
    return;
  using IntFn = int(PROXY_THISCALL *)(void *);
  using GetEntityFn = const uint8_t *(PROXY_THISCALL *)(void *, int);
  using GetClassFn = const void *(PROXY_THISCALL *)(const void *);
  void **engine = *static_cast<void ***>(g_engineClient);
  const int local = reinterpret_cast<IntFn>(engine[12])(g_engineClient);
  const int players = reinterpret_cast<IntFn>(engine[21])(g_engineClient);
  const auto getEntity = reinterpret_cast<GetEntityFn>((*static_cast<void ***>(g_entityList))[3]);
  std::vector<Flashlight> lit;
  for (int i = 1; i <= players && i < 256; ++i) {
    const uint8_t *entity = getEntity(g_entityList, i);
    if (!entity)
      continue;
    // IClientNetworkable at +8, slot 1: GetClientClass.
    const void *networkable = entity + 8;
    const void *clientClass = reinterpret_cast<GetClassFn>((*static_cast<void *const *const *>(networkable))[1])(networkable);
    if (clientClass != g_terrorPlayerClass && clientClass != g_survivorBotClass)
      continue;
    Flashlight f{i, {}, {}};
    float right[3], up[3];
    const float *offset;
    if (i == local && clientClass == g_terrorPlayerClass) {
      if (!read<bool>(entity, kLocalOn))
        continue;
      std::memcpy(f.position, entity + kLocalEye, 12);
      std::memcpy(f.forward, entity + kLocalForward, 12);
      std::memcpy(up, entity + kLocalUp, 12);
      std::memcpy(right, entity + kLocalRight, 12);
      offset = kPlayerOffset;
    } else {
      if (!(read<int>(entity, g_effects) & 4) || g_origin <= 0 || g_viewOffset <= 0) // EF_DIMLIGHT
        continue;
      float origin[3], eye[3], angles[3];
      std::memcpy(origin, entity + g_origin, 12);
      std::memcpy(eye, entity + g_viewOffset, 12);
      std::memcpy(angles, entity + g_eyeAngles, 12);
      if (!finite3(angles))
        continue;
      for (int k = 0; k < 3; ++k)
        f.position[k] = origin[k] + eye[k];
      angleVectors(angles, f.forward, right, up);
      offset = kBotOffset;
    }
    if (!finite3(f.position) || !finite3(right) || !finite3(up) || !normalize(f.forward))
      continue;
    for (int k = 0; k < 3; ++k)
      f.position[k] += f.forward[k] * offset[0] + right[k] * offset[2] + up[k] * offset[1];
    lit.push_back(f);
  }
  ++g_stats.samples;
  g_stats.lit += lit.size();
  std::lock_guard<std::mutex> lock(g_lock);
  g_sampled = std::move(lit);
}

void submitLights(const remixapi_Interface &api) {
  if (!g_entityList)
    return;
  std::vector<Flashlight> lit;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    lit = g_sampled;
  }
  for (auto &[key, light] : g_lights)
    light.seen = false;
  for (const Flashlight &f : lit)
    for (int inner = 0; inner < 2; ++inner) {
      const Cone &cone = inner ? kInner : kOuter;
      remixapi_LightInfoSphereEXT sphere = {};
      sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
      sphere.position = {f.position[0], f.position[1], f.position[2]};
      sphere.radius = cone.radius;
      sphere.volumetricRadianceScale = cone.volumetric;
      sphere.shaping_hasvalue = 1;
      sphere.shaping_value.direction = {f.forward[0], f.forward[1], f.forward[2]};
      sphere.shaping_value.coneAngleDegrees = cone.angle;
      sphere.shaping_value.coneSoftness = cone.softness;
      sphere.shaping_value.focusExponent = cone.exponent;
      remixapi_LightInfo info = {};
      info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
      info.pNext = &sphere;
      const char tag[] = "openremix:l4d2:flashlight";
      const int id[2] = {f.index, inner};
      info.hash = fnv(id, sizeof(id), fnv(tag, sizeof(tag)));
      const float radiance = 20.0f * cone.intensity * g_scale;
      info.radiance = {radiance, radiance, radiance};
      const uint64_t definition = fnv(&sphere.position, sizeof(sphere) - offsetof(remixapi_LightInfoSphereEXT, position),
                                      fnv(&radiance, sizeof(radiance)));
      Light &light = g_lights[f.index * 2 + inner];
      light.seen = true;
      if (!light.handle) {
        if (api.CreateLight(&info, &light.handle) != REMIXAPI_ERROR_CODE_SUCCESS || !light.handle) {
          light.handle = nullptr;
          ++g_stats.failed;
          continue;
        }
        ++g_stats.created;
      } else if (light.definition != definition) {
        // Updated in place: a new light every frame changed the scene's light
        // set (light tree rebuilds, flicker; l4d2-rtx).
        const remixapi_ErrorCode r = api.UpdateLightDefinition ? api.UpdateLightDefinition(light.handle, &info)
                                                               : api.CreateLight(&info, &light.handle);
        if (r == REMIXAPI_ERROR_CODE_SUCCESS)
          ++g_stats.updated;
        else
          ++g_stats.failed;
      }
      light.definition = definition;
      api.DrawLightInstance(light.handle);
      ++g_stats.drawn;
    }
  for (auto it = g_lights.begin(); it != g_lights.end();) {
    if (it->second.seen) {
      ++it;
      continue;
    }
    if (it->second.handle) {
      api.DestroyLight(it->second.handle);
      ++g_stats.destroyed;
    }
    it = g_lights.erase(it);
  }
}

void dumpLights(FILE *out) {
  static ULONGLONG last = GetTickCount64();
  const ULONGLONG now = GetTickCount64();
  const double seconds = now > last ? double(now - last) / 1000 : 1;
  last = now;
  const uint64_t samples = g_stats.samples.exchange(0);
  fprintf(out,
          "== L4D2 flashlights: %s (scale %.2f; netvars m_fEffects +0x%X m_angEyeAngles +0x%X m_vecOrigin +0x%X "
          "m_vecViewOffset +0x%X)\n",
          g_status.load(), g_scale, g_effects, g_eyeAngles, g_origin, g_viewOffset);
  fprintf(out,
          "   per second: samples %.1f, lit flashlights per sample %.2f | lights created %.1f, updated %.1f, drawn "
          "%.1f, destroyed %.1f, failed %.1f\n",
          double(samples) / seconds, samples ? double(g_stats.lit.exchange(0)) / double(samples) : 0.0,
          double(g_stats.created.exchange(0)) / seconds, double(g_stats.updated.exchange(0)) / seconds,
          double(g_stats.drawn.exchange(0)) / seconds, double(g_stats.destroyed.exchange(0)) / seconds,
          double(g_stats.failed.exchange(0)) / seconds);
}

} // namespace proxy::l4d2
