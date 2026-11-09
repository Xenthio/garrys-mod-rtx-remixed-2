// x64 bridge host: owns the OpenRemix runtime on a single thread and
// replays the command ring written by the x86 client in the game process.
#include <remix/remix_c.h>
#include <remix/remix_native_ext.h>
#include <remix/remix_usd_ext.h>
#include <remix/remix_reconstruction_ext.h>

#include "common/layouts.gen.h"
#include "common/platform.h"
#include "common/protocol.h"
#include "host/input.h"
#include "host/profiler.h"

#include <shellapi.h>

#include <cstring>
#include <algorithm>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

using namespace bridge;
using namespace bridge::layout;
using namespace bridge::proto;

namespace {

Log g_log;
bool g_trace = false;

struct HandleTable {
  std::vector<void *> slots;
  void set(uint64_t id, void *h) {
    if (id >= slots.size())
      slots.resize(id + 1024, nullptr);
    slots[id] = h;
  }
  void *get(uint64_t id) const { return id < slots.size() ? slots[id] : nullptr; }
  void *take(uint64_t id) {
    void *h = get(id);
    if (h)
      slots[id] = nullptr;
    return h;
  }
};

template <class Fn> Fn proc(HMODULE lib, const char *name) {
  return reinterpret_cast<Fn>(reinterpret_cast<void *>(GetProcAddress(lib, name)));
}

// Upscaler status (requested mode, active backend, fallback reason), logged
// whenever it changes: the runtime shows it only in its own UI.
HMODULE g_runtimeLibrary = nullptr;
void logReconstruction() {
  using Fn = remixapi_ErrorCode(REMIXAPI_CALL *)(remixapi_ReconstructionDiagnosticsEXT *);
  static Fn get = nullptr;
  static int frames = 0;
  static std::string last;
  if (!g_runtimeLibrary || ++frames % 120 != 1)
    return;
  if (!get)
    get = reinterpret_cast<Fn>(
        reinterpret_cast<void *>(GetProcAddress(g_runtimeLibrary, "remixapi_GetReconstructionDiagnosticsEXT")));
  if (!get)
    return;
  remixapi_ReconstructionDiagnosticsEXT d = {};
  d.structSize = sizeof(d);
  if (get(&d) != REMIXAPI_ERROR_CODE_SUCCESS)
    return;
  char line[512];
  snprintf(line, sizeof line,
           "host: reconstruction requested %u active %u caps 0x%x render %ux%u display %ux%u failures %llu %s",
           d.requestedMode, d.activeBackend, d.capabilities, d.renderWidth, d.renderHeight, d.displayWidth,
           d.displayHeight, static_cast<unsigned long long>(d.failures), d.fallbackReason);
  if (last != line) {
    last = line;
    g_log.write("%s", line);
  }
}

class Host {
public:
  bool open(DWORD clientPid, DWORD nonce);
  bool loadRuntime();
  int run();

private:
  // Pointer resolution for the record being executed.
  void *resolve(uint64_t v);
  void fixup(StructId id, uint8_t *image);
  template <class T> T *arg(uint64_t encoded, StructId id) {
    auto *p = static_cast<uint8_t *>(resolve(encoded));
    if (p)
      fixup(id, p);
    return reinterpret_cast<T *>(p);
  }

  void execute(const Record *rec, uint32_t size);
  void reply(uint64_t seq, int32_t result, const void *payload = nullptr, uint32_t size = 0);
  uint8_t *replyPayload() { return m_reply + sizeof(Reply); }
  void asyncResult(Op op, remixapi_ErrorCode code);
  void release(uint64_t pos);

  Header *m_header = nullptr;
  uint8_t *m_reply = nullptr;
  uint8_t *m_ring = nullptr;
  uint32_t m_ringSize = 0;
  HANDLE m_work = nullptr, m_done = nullptr, m_client = nullptr;

  const uint8_t *m_recBase = nullptr;
  uint32_t m_recSize = 0;
  std::unordered_map<uint64_t, std::vector<uint8_t>> m_blobs;
  std::unordered_map<uint64_t, std::vector<uint8_t>> m_batches; // StoreBatch
  std::vector<uint8_t> m_replay; // ReplayBatch: fixup() decodes records in place
  // Dev aid (OPENREMIX_BRIDGE_LIGHT_LOG=1): light instances per frame.
  std::vector<uint64_t> m_frameLights, m_lastFrameLights;
  uint64_t m_lightLogFrames = 0, m_lightSetChanges = 0, m_lightMissing = 0, m_lightCreates = 0, m_lightDestroys = 0;
  std::map<size_t, uint64_t> m_lightCounts;
  void logLights();
  HandleTable m_materials, m_meshes, m_lights, m_textures;
  // Client id -> runtime handle (0 = none: never created or refused).
  struct IdMap {
    std::vector<uint64_t> slots;
    void set(uint64_t id, uint64_t h) {
      if (id >= slots.size())
        slots.resize(id + 1024, 0);
      slots[id] = h;
    }
    uint64_t get(uint64_t id) const { return id < slots.size() ? slots[id] : 0; }
    uint64_t take(uint64_t id) {
      const uint64_t h = get(id);
      if (h)
        slots[id] = 0;
      return h;
    }
  } m_sceneInstances, m_hostShaders;
  void refreshCachedGetters();
  uint64_t m_failures = 0;

  HMODULE m_lib = nullptr;
  remixapi_Interface m_api = {};
  PFN_remixapi_PollInputEvents m_pollInput = nullptr;
  PFN_remixapi_SetTextInputEnabled m_textInput = nullptr;
  PFN_remixapi_GetWindowMetrics m_metrics = nullptr;
  PFN_remixapi_GetPresentationCount m_presentations = nullptr;
  PFN_remixapi_SetConfigDefaultEXT m_configDefault = nullptr;
  PFN_remixapi_UpdateTextureEXT m_updateTexture = nullptr;
  PFN_remixapi_UpdateUITextureRegionEXT m_updateUIRegion = nullptr;
  PFN_remixapi_AddSceneInstancesEXT m_addScene = nullptr;
  PFN_remixapi_SetSceneInstanceEXT m_setScene = nullptr;
  PFN_remixapi_ChangeSceneInstancesEXT m_changeScene = nullptr;
  PFN_remixapi_RemoveSceneInstancesEXT m_removeScene = nullptr;
  PFN_remixapi_DrawSceneInstancesEXT m_drawScene = nullptr;
  PFN_remixapi_RegisterHostShaderEXT m_registerShader = nullptr;
  PFN_remixapi_DestroyHostShaderEXT m_destroyShader = nullptr;
  PFN_remixapi_SubmitHostPassesEXT m_submitPasses = nullptr;
  PFN_remixapi_UpdateHostTargetEXT m_updateTarget = nullptr;
  PFN_remixapi_UpdateHostVolumeEXT m_updateVolume = nullptr;
  PFN_remixapi_GetHostPassesEnabledEXT m_passesEnabled = nullptr;
  PFN_remixapi_GetHostPassStatisticsEXT m_passStats = nullptr;
  std::vector<uint64_t> m_handleScratch;
  remixapi_ErrorCode(REMIXAPI_CALL *m_configureMods)(const remixapi_USDModsOptionsEXT *) = nullptr;
  PFN_remixapi_SetVisibilityPartitionEXT m_setVisibility = nullptr;
  remixapi_ErrorCode(REMIXAPI_CALL *m_modsDiagnostic)(char *, uint32_t) = nullptr;
  using StatFn = remixapi_ErrorCode(REMIXAPI_CALL *)(void *);
  StatFn m_stats[static_cast<size_t>(StatKind::Count)] = {};
};

bool Host::open(DWORD clientPid, DWORD nonce) {
  HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, objectName(clientPid, nonce, L"shm").c_str());
  if (!mapping) {
    g_log.write("host: OpenFileMapping failed (%lu)", GetLastError());
    return false;
  }
  auto *base = static_cast<uint8_t *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
  if (!base)
    return false;
  m_header = reinterpret_cast<Header *>(base);
  if (m_header->magic != kMagic || m_header->version != kVersion) {
    g_log.write("host: protocol mismatch (magic %08x version %u)", m_header->magic, m_header->version);
    return false;
  }
  m_reply = base + m_header->headerSize;
  m_ring = m_reply + m_header->replySize;
  m_ringSize = m_header->ringSize;
  m_work = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, objectName(clientPid, nonce, L"work").c_str());
  m_done = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, objectName(clientPid, nonce, L"done").c_str());
  m_client = OpenProcess(SYNCHRONIZE, FALSE, clientPid);
  m_header->hostPid = GetCurrentProcessId();
  return m_work && m_done && m_client;
}

bool Host::loadRuntime() {
  const std::wstring path = moduleDirectory(nullptr) + L"\\remix.dll";
  const auto status = remixapi_lib_loadRemixDllAndInitialize(path.c_str(), &m_api, &m_lib);
  if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
    snprintf(m_header->lastError, sizeof(m_header->lastError), "loading remix.dll failed: %d", int(status));
    g_log.write("host: %s", m_header->lastError);
    m_header->hostState.store(static_cast<uint32_t>(HostState::Failed));
    return false;
  }
  // The runtime reports failure causes (Vulkan errors, device loss) through
  // SDL's log, whose default output has no console here: copy it to our log.
  if (HMODULE sdl = GetModuleHandleW(L"SDL3.dll")) {
    using LogFn = void (*)(void *, int, int, const char *);
    using SetLogFn = void (*)(LogFn, void *);
    if (auto setLog = proc<SetLogFn>(sdl, "SDL_SetLogOutputFunction"))
      setLog([](void *, int, int, const char *message) { g_log.write("runtime: %s", message); }, nullptr);
  }
  g_runtimeLibrary = m_lib;
  m_pollInput = proc<PFN_remixapi_PollInputEvents>(m_lib, "remixapi_PollInputEvents");
  m_textInput = proc<PFN_remixapi_SetTextInputEnabled>(m_lib, "remixapi_SetTextInputEnabled");
  m_metrics = proc<PFN_remixapi_GetWindowMetrics>(m_lib, "remixapi_GetWindowMetrics");
  m_presentations = proc<PFN_remixapi_GetPresentationCount>(m_lib, "remixapi_GetPresentationCount");
  m_configDefault = proc<decltype(m_configDefault)>(m_lib, "remixapi_SetConfigDefaultEXT");
  m_updateTexture = proc<decltype(m_updateTexture)>(m_lib, "remixapi_UpdateTextureEXT");
  m_updateUIRegion = proc<decltype(m_updateUIRegion)>(m_lib, "remixapi_UpdateUITextureRegionEXT");
  m_addScene = proc<decltype(m_addScene)>(m_lib, "remixapi_AddSceneInstancesEXT");
  m_setScene = proc<decltype(m_setScene)>(m_lib, "remixapi_SetSceneInstanceEXT");
  m_changeScene = proc<decltype(m_changeScene)>(m_lib, "remixapi_ChangeSceneInstancesEXT");
  m_removeScene = proc<decltype(m_removeScene)>(m_lib, "remixapi_RemoveSceneInstancesEXT");
  m_drawScene = proc<decltype(m_drawScene)>(m_lib, "remixapi_DrawSceneInstancesEXT");
  m_registerShader = proc<decltype(m_registerShader)>(m_lib, "remixapi_RegisterHostShaderEXT");
  m_destroyShader = proc<decltype(m_destroyShader)>(m_lib, "remixapi_DestroyHostShaderEXT");
  m_submitPasses = proc<decltype(m_submitPasses)>(m_lib, "remixapi_SubmitHostPassesEXT");
  m_updateTarget = proc<decltype(m_updateTarget)>(m_lib, "remixapi_UpdateHostTargetEXT");
  m_updateVolume = proc<decltype(m_updateVolume)>(m_lib, "remixapi_UpdateHostVolumeEXT");
  m_passesEnabled = proc<decltype(m_passesEnabled)>(m_lib, "remixapi_GetHostPassesEnabledEXT");
  m_passStats = proc<decltype(m_passStats)>(m_lib, "remixapi_GetHostPassStatisticsEXT");
  m_header->hostPassesEnabled.store(kHostPassesUnavailable);
  m_header->runtimeExports = (m_configDefault ? kExportConfigDefault : 0u) |
                             (m_updateTexture ? kExportUpdateTexture : 0u) |
                             (m_updateUIRegion ? kExportUpdateUITextureRegion : 0u) |
                             (m_addScene && m_setScene && m_changeScene && m_removeScene && m_drawScene
                                  ? kExportSceneInstances
                                  : 0u) |
                             (m_registerShader && m_destroyShader && m_submitPasses && m_updateTarget
                                  ? kExportHostPasses
                                  : 0u) |
                             (m_updateVolume ? kExportHostVolume : 0u) |
                             (m_passStats ? kExportHostPassStatistics : 0u);
  m_header->hostPassStatsResult.store(m_passStats ? REMIXAPI_ERROR_CODE_NOT_INITIALIZED
                                                  : REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE);
  m_configureMods = proc<decltype(m_configureMods)>(m_lib, "remixapi_ConfigureUSDModsEXT");
  m_setVisibility = proc<decltype(m_setVisibility)>(m_lib, "remixapi_SetVisibilityPartitionEXT");
  m_modsDiagnostic = proc<decltype(m_modsDiagnostic)>(m_lib, "remixapi_GetUSDModsDiagnosticEXT");
  static const char *const statNames[] = {
      "remixapi_GetBackendCapabilitiesEXT", "remixapi_GetLocalLightStatisticsEXT",
      "remixapi_GetLocalLightGuidingStatisticsEXT", "remixapi_GetShadowStatisticsEXT",
      "remixapi_GetSharcStatisticsEXT", "remixapi_GetTracingStatisticsEXT", "remixapi_GetFramePerformanceEXT",
      "remixapi_GetFrameBreakdownEXT", "remixapi_GetGpuPassTimingsEXT", "remixapi_GetDenoisingCapabilitiesEXT",
      "remixapi_GetDenoisingStatisticsEXT", "remixapi_GetTextureStatisticsEXT",
      "remixapi_GetCameraCullingStatisticsEXT", "remixapi_GetLocalLightSetStatisticsEXT"};
  static_assert(sizeof(statNames) / sizeof(statNames[0]) == static_cast<size_t>(StatKind::Count));
  for (size_t i = 0; i < static_cast<size_t>(StatKind::Count); ++i)
    m_stats[i] = proc<StatFn>(m_lib, statNames[i]);
  m_header->hostState.store(static_cast<uint32_t>(HostState::Ready));
  g_log.write("host: runtime loaded, ready");
  return true;
}

// The header's cached answers for per-frame getters (proto::Header).
void Host::refreshCachedGetters() {
  if (m_passesEnabled) {
    remixapi_Bool on = 0;
    if (m_passesEnabled(&on) == REMIXAPI_ERROR_CODE_SUCCESS)
      m_header->hostPassesEnabled.store(on ? 1u : 0u, std::memory_order_release);
  }
  if (m_passStats) {
    remixapi_HostPassStatisticsEXT stats = {};
    stats.structSize = sizeof(stats);
    const remixapi_ErrorCode r = m_passStats(&stats);
    const uint64_t seq = m_header->hostPassStatsSeq.load(std::memory_order_relaxed);
    m_header->hostPassStatsSeq.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    m_header->hostPassStatsResult.store(r, std::memory_order_relaxed);
    std::memcpy(m_header->hostPassStats, &stats, sizeof(stats));
    m_header->hostPassStatsSeq.store(seq + 2, std::memory_order_release);
  }
}

void *Host::resolve(uint64_t v) {
  if (v == 0)
    return nullptr;
  const uint64_t off = v & ~kPtrTagMask;
  switch (v & kPtrTagMask) {
  case kPtrRecord:
    return off < m_recSize ? const_cast<uint8_t *>(m_recBase) + off : nullptr;
  case kPtrRing:
    return m_ring + off % m_ringSize;
  case kPtrBlob: {
    auto it = m_blobs.find(off);
    return it == m_blobs.end() ? nullptr : it->second.data();
  }
  default:
    return nullptr;
  }
}

void Host::fixup(StructId id, uint8_t *image) {
  const StructDesc &d = kStructs[id];
  for (uint16_t i = 0; i < d.fieldCount; ++i) {
    const FieldDesc &f = d.fields[i];
    uint8_t *field = image + f.off64;
    uint64_t v;
    std::memcpy(&v, field, 8 <= f.size64 ? 8 : f.size64);
    auto setPtr = [&](const void *p) { std::memcpy(field, &p, 8); };
    switch (f.kind) {
    case F_POD:
    case F_HWND:
      break;
    case F_NESTED:
      fixup(static_cast<StructId>(f.aux), field);
      break;
    case F_PNEXT: {
      auto *p = static_cast<uint8_t *>(resolve(v));
      setPtr(p);
      if (p) {
        uint32_t sType;
        std::memcpy(&sType, p, 4);
        for (uint16_t s = 0; s < S_COUNT; ++s) {
          if (kStructs[s].sType == sType) {
            fixup(static_cast<StructId>(s), p);
            break;
          }
        }
      }
      break;
    }
    case F_HANDLE_MATERIAL:
      setPtr(m_materials.get(v));
      break;
    case F_HANDLE_MESH:
      setPtr(m_meshes.get(v));
      break;
    case F_HANDLE_HOST_SHADER: {
      const uint64_t h = m_hostShaders.get(v);
      std::memcpy(field, &h, 8);
      break;
    }
    case F_ARRAY_STRUCT: {
      auto *p = static_cast<uint8_t *>(resolve(v));
      setPtr(p);
      const FieldDesc &c = d.fields[f.link];
      uint64_t count = 0;
      std::memcpy(&count, image + c.off64, c.size64);
      const StructDesc &e = kStructs[f.aux];
      for (uint64_t k = 0; p && k < count; ++k)
        fixup(static_cast<StructId>(f.aux), p + k * e.size64);
      break;
    }
    case F_WSTR:
    case F_CSTR:
    case F_ARRAY:
    case F_BLOB:
    case F_OPTIONAL:
    case F_NULLPTR:
      setPtr(resolve(v));
      break;
    }
  }
}

void Host::reply(uint64_t seq, int32_t result, const void *payload, uint32_t size) {
  if (!seq)
    return;
  auto *r = reinterpret_cast<Reply *>(m_reply);
  if (payload && size && payload != replyPayload())
    std::memcpy(replyPayload(), payload, size);
  r->seq = seq;
  r->result = result;
  r->size = size;
  m_header->syncDone.store(seq, std::memory_order_release);
  if (m_header->clientSleeping.exchange(0))
    SetEvent(m_done);
}

void Host::asyncResult(Op op, remixapi_ErrorCode code) {
  if (code == REMIXAPI_ERROR_CODE_SUCCESS)
    return;
  ++m_failures;
  m_header->asyncErrors.fetch_add(1);
  if (m_failures <= 64 || m_failures % 1024 == 0) {
    snprintf(m_header->lastError, sizeof(m_header->lastError), "%s failed: %d (failure #%llu)", opName(op),
             int(code), static_cast<unsigned long long>(m_failures));
    g_log.write("host: %s", m_header->lastError);
  }
}

void Host::release(uint64_t pos) {
  m_header->readPos.store(pos, std::memory_order_release);
  if (m_header->clientSleeping.exchange(0))
    SetEvent(m_done);
}

void Host::logLights() {
  static const bool enabled = [] {
    char v[8] = {};
    return GetEnvironmentVariableA("OPENREMIX_BRIDGE_LIGHT_LOG", v, sizeof(v)) && v[0] == '1';
  }();
  if (!enabled) {
    m_frameLights.clear();
    return;
  }
  std::sort(m_frameLights.begin(), m_frameLights.end());
  ++m_lightCounts[m_frameLights.size()];
  if (m_frameLights != m_lastFrameLights)
    ++m_lightSetChanges;
  m_lastFrameLights.swap(m_frameLights);
  m_frameLights.clear();
  if (++m_lightLogFrames % 600)
    return;
  std::string counts;
  for (auto [n, frames] : m_lightCounts)
    counts += " " + std::to_string(n) + "x" + std::to_string(frames);
  g_log.write("host: lights over 600 frames: set changes %llu, draws of unknown handles %llu, creates %llu, "
              "destroys %llu; lights per frame (count x frames):%s",
              (unsigned long long)m_lightSetChanges, (unsigned long long)m_lightMissing,
              (unsigned long long)m_lightCreates, (unsigned long long)m_lightDestroys, counts.c_str());
  m_lightCounts.clear();
  m_lightSetChanges = m_lightMissing = m_lightCreates = m_lightDestroys = 0;
}

void Host::execute(const Record *rec, uint32_t size) {
  m_recBase = reinterpret_cast<const uint8_t *>(rec);
  m_recSize = size;
  const void *args = rec + 1;
  auto &api = m_api;
  const Op op = rec->op;
  const uint64_t seq = rec->seq;
  remixapi_ErrorCode r = REMIXAPI_ERROR_CODE_SUCCESS;
  auto A = [&](auto *type) -> decltype(auto) {
    return *static_cast<const std::remove_pointer_t<decltype(type)> *>(args);
  };

  switch (op) {
  case Op::Indirect: {
    auto it = m_blobs.find(A((ArgsHandle *)nullptr).id);
    if (it == m_blobs.end() || it->second.size() < sizeof(Record)) {
      asyncResult(op, REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS);
      reply(seq, REMIXAPI_ERROR_CODE_GENERAL_FAILURE);
      return;
    }
    auto &blob = it->second;
    execute(reinterpret_cast<const Record *>(blob.data()), static_cast<uint32_t>(blob.size()));
    return;
  }
  case Op::Startup:
    r = api.Startup(arg<remixapi_StartupInfo>(A((ArgsInfo *)nullptr).info, S_StartupInfo));
    g_log.write("host: Startup -> %d", int(r));
    if (r == REMIXAPI_ERROR_CODE_SUCCESS)
      refreshCachedGetters();
    reply(seq, r);
    return;
  case Op::Shutdown:
    r = api.Shutdown();
    reply(seq, r);
    return;
  case Op::Present:
    // Injected UI input (InjectInput) reaches the runtime's UI through its
    // SDL event queue, which nothing else pumps for a borrowed window.
    if (takeInputPending())
      api.PumpEvents();
    logReconstruction();
    r = api.Present(arg<remixapi_PresentInfo>(A((ArgsInfo *)nullptr).info, S_PresentInfo));
    refreshCachedGetters();
    m_header->framesPresented.fetch_add(1, std::memory_order_release);
    logLights();
    break;
  case Op::CreateMaterial: {
    const auto &a = A((ArgsCreate *)nullptr);
    remixapi_MaterialHandle h = nullptr;
    r = api.CreateMaterial(arg<remixapi_MaterialInfo>(a.info, S_MaterialInfo), &h);
    m_materials.set(a.id, h);
    break;
  }
  case Op::DestroyMaterial:
    if (void *h = m_materials.take(A((ArgsHandle *)nullptr).id))
      r = api.DestroyMaterial(static_cast<remixapi_MaterialHandle>(h));
    break;
  case Op::CreateMesh: {
    const auto &a = A((ArgsCreate *)nullptr);
    remixapi_MeshHandle h = nullptr;
    r = api.CreateMesh(arg<remixapi_MeshInfo>(a.info, S_MeshInfo), &h);
    m_meshes.set(a.id, h);
    break;
  }
  case Op::DestroyMesh:
    if (void *h = m_meshes.take(A((ArgsHandle *)nullptr).id))
      r = api.DestroyMesh(static_cast<remixapi_MeshHandle>(h));
    break;
  case Op::SetupCamera:
    r = api.SetupCamera(arg<remixapi_CameraInfo>(A((ArgsInfo *)nullptr).info, S_CameraInfo));
    break;
  case Op::DrawInstance: {
    auto *info = arg<remixapi_InstanceInfo>(A((ArgsInfo *)nullptr).info, S_InstanceInfo);
    if (info && info->mesh)
      r = api.DrawInstance(info);
    break;
  }
  case Op::CreateLight: {
    const auto &a = A((ArgsCreate *)nullptr);
    remixapi_LightHandle h = nullptr;
    r = api.CreateLight(arg<remixapi_LightInfo>(a.info, S_LightInfo), &h);
    m_lights.set(a.id, h);
    ++m_lightCreates;
    break;
  }
  case Op::DestroyLight:
    if (void *h = m_lights.take(A((ArgsHandle *)nullptr).id))
      r = api.DestroyLight(static_cast<remixapi_LightHandle>(h));
    ++m_lightDestroys;
    break;
  case Op::DrawLightInstance:
    m_frameLights.push_back(A((ArgsHandle *)nullptr).id);
    if (void *h = m_lights.get(A((ArgsHandle *)nullptr).id))
      r = api.DrawLightInstance(static_cast<remixapi_LightHandle>(h));
    else
      ++m_lightMissing;
    break;
  case Op::UpdateLightDefinition: {
    const auto &a = A((ArgsLightUpdate *)nullptr);
    if (void *h = m_lights.get(a.id))
      r = api.UpdateLightDefinition(static_cast<remixapi_LightHandle>(h),
                                    arg<remixapi_LightInfo>(a.info, S_LightInfo));
    break;
  }
  case Op::AutoInstancePersistentLights:
    r = api.AutoInstancePersistentLights();
    break;
  case Op::SetConfigVariable: {
    const auto &a = A((ArgsStrings *)nullptr);
    r = api.SetConfigVariable(static_cast<const char *>(resolve(a.a)), static_cast<const char *>(resolve(a.b)));
    refreshCachedGetters();
    reply(seq, r);
    return;
  }
  case Op::SetConfigDefault: {
    const auto &a = A((ArgsStrings *)nullptr);
    r = m_configDefault
            ? m_configDefault(static_cast<const char *>(resolve(a.a)), static_cast<const char *>(resolve(a.b)))
            : REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    refreshCachedGetters();
    reply(seq, r);
    return;
  }
  case Op::UpdateTexture: {
    const auto &a = A((ArgsRegion *)nullptr);
    if (!m_updateTexture) {
      r = REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    } else if (void *h = m_textures.get(a.handle)) {
      r = m_updateTexture(static_cast<remixapi_TextureHandle>(h), a.x, a.y, a.width, a.height,
                          static_cast<remixapi_Format>(a.format), resolve(a.pixels), a.rowPitch);
    }
    break;
  }
  case Op::UpdateUITextureRegion: {
    const auto &a = A((ArgsRegion *)nullptr);
    r = m_updateUIRegion ? m_updateUIRegion(a.handle, a.x, a.y, a.width, a.height,
                                            static_cast<remixapi_Format>(a.format), resolve(a.pixels), a.rowPitch)
                         : REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    break;
  }
  case Op::AddSceneInstances: {
    const auto &a = A((ArgsSceneAdd *)nullptr);
    const auto *ids = static_cast<const uint64_t *>(resolve(a.ids));
    auto *infos = static_cast<uint8_t *>(resolve(a.infos));
    if (!ids || !infos) {
      r = REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
      break;
    }
    for (uint32_t i = 0; i < a.count; ++i)
      fixup(S_InstanceInfo, infos + size_t(i) * sizeof(remixapi_InstanceInfo));
    m_handleScratch.assign(a.count, 0);
    r = m_addScene ? m_addScene(reinterpret_cast<const remixapi_InstanceInfo *>(infos), a.count, m_handleScratch.data())
                   : REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    for (uint32_t i = 0; i < a.count; ++i)
      m_sceneInstances.set(ids[i], r == REMIXAPI_ERROR_CODE_SUCCESS ? m_handleScratch[i] : 0);
    break;
  }
  case Op::SetSceneInstance: {
    const auto &a = A((ArgsCreate *)nullptr);
    if (const uint64_t h = m_sceneInstances.get(a.id))
      r = m_setScene ? m_setScene(h, arg<remixapi_InstanceInfo>(a.info, S_InstanceInfo))
                     : REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    break;
  }
  case Op::ChangeSceneInstances: {
    const auto &a = A((ArgsArray *)nullptr);
    auto *changes = static_cast<remixapi_SceneInstanceChangeEXT *>(resolve(a.items));
    if (!changes || !m_changeScene) {
      r = m_changeScene ? REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS : REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
      break;
    }
    // Translate in place, dropping changes of instances that were refused.
    uint32_t n = 0;
    for (uint32_t i = 0; i < a.count; ++i) {
      if (const uint64_t h = m_sceneInstances.get(changes[i].instance)) {
        changes[n] = changes[i];
        changes[n++].instance = h;
      }
    }
    if (n)
      r = m_changeScene(changes, n);
    break;
  }
  case Op::RemoveSceneInstances: {
    const auto &a = A((ArgsArray *)nullptr);
    auto *ids = static_cast<uint64_t *>(resolve(a.items));
    if (!ids || !m_removeScene) {
      r = m_removeScene ? REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS : REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
      break;
    }
    uint32_t n = 0;
    for (uint32_t i = 0; i < a.count; ++i)
      if (const uint64_t h = m_sceneInstances.take(ids[i]))
        ids[n++] = h;
    if (n)
      r = m_removeScene(ids, n);
    break;
  }
  case Op::DrawSceneInstances: {
    const auto &a = A((ArgsOrigin *)nullptr);
    r = m_drawScene ? m_drawScene(a.hasOrigin ? a.origin : nullptr) : REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    break;
  }
  case Op::RegisterHostShader: {
    const auto &a = A((ArgsCreate *)nullptr);
    remixapi_HostShaderHandleEXT h = 0;
    r = m_registerShader ? m_registerShader(arg<remixapi_HostShaderInfoEXT>(a.info, S_HostShaderInfoEXT), &h)
                         : REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    m_hostShaders.set(a.id, r == REMIXAPI_ERROR_CODE_SUCCESS ? h : 0);
    break;
  }
  case Op::DestroyHostShader:
    if (const uint64_t h = m_hostShaders.take(A((ArgsHandle *)nullptr).id))
      r = m_destroyShader ? m_destroyShader(h) : REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    break;
  case Op::SubmitHostPasses:
    r = m_submitPasses ? m_submitPasses(arg<remixapi_HostPassListEXT>(A((ArgsInfo *)nullptr).info, S_HostPassListEXT))
                       : REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    break;
  case Op::UpdateHostTarget:
  case Op::UpdateHostVolume: {
    const auto &a = A((ArgsHostImage *)nullptr);
    if (op == Op::UpdateHostTarget)
      r = m_updateTarget ? m_updateTarget(a.target, a.width, a.height, a.format, resolve(a.pixels), a.bytes)
                         : REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    else
      r = m_updateVolume ? m_updateVolume(a.target, a.width, a.height, a.depth, a.format, resolve(a.pixels), a.bytes)
                         : REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    break;
  }
  case Op::AddTextureHash:
  case Op::RemoveTextureHash: {
    const auto &a = A((ArgsStrings *)nullptr);
    auto fn = op == Op::AddTextureHash ? api.AddTextureHash : api.RemoveTextureHash;
    r = fn(static_cast<const char *>(resolve(a.a)), static_cast<const char *>(resolve(a.b)));
    break;
  }
  case Op::CreateTexture: {
    const auto &a = A((ArgsCreate *)nullptr);
    remixapi_TextureHandle h = nullptr;
    r = api.CreateTexture(arg<remixapi_TextureInfo>(a.info, S_TextureInfo), &h);
    m_textures.set(a.id, h);
    break;
  }
  case Op::DestroyTexture:
    if (void *h = m_textures.take(A((ArgsHandle *)nullptr).id))
      r = api.DestroyTexture(static_cast<remixapi_TextureHandle>(h));
    break;
  case Op::GetUIState: {
    const uint32_t ui = api.GetUIState();
    reply(seq, REMIXAPI_ERROR_CODE_SUCCESS, &ui, sizeof(ui));
    return;
  }
  case Op::SetUIState:
    r = api.SetUIState(static_cast<remixapi_UIState>(A((ArgsU32 *)nullptr).value));
    break;
  case Op::DrawScreenOverlay: {
    const auto &a = A((ArgsOverlay *)nullptr);
    r = api.DrawScreenOverlay(resolve(a.pixels), a.width, a.height, static_cast<remixapi_Format>(a.format),
                              a.opacity);
    break;
  }
  case Op::SetGameValue: {
    const auto &a = A((ArgsStrings *)nullptr);
    r = api.SetGameValue(static_cast<const char *>(resolve(a.a)), static_cast<const char *>(resolve(a.b)));
    break;
  }
  case Op::GetGameValue: {
    const auto &a = A((ArgsGameValue *)nullptr);
    uint32_t capacity = a.capacity < 4096 ? a.capacity : 4096;
    uint32_t length = 0;
    char *out = reinterpret_cast<char *>(replyPayload() + 4);
    r = api.GetGameValue(static_cast<const char *>(resolve(a.key)), capacity ? out : nullptr, capacity, &length);
    std::memcpy(replyPayload(), &length, 4);
    reply(seq, r, replyPayload(), 4 + (capacity ? static_cast<uint32_t>(strnlen(out, capacity)) + 1 : 0));
    return;
  }
  case Op::RequestVramCompaction:
    r = api.RequestVramCompaction();
    break;
  case Op::GetVramStats: {
    remixapi_VramStats stats = {};
    r = api.GetVramStats(&stats);
    reply(seq, r, &stats, sizeof(stats));
    return;
  }
  case Op::RequestTextureVramFree:
    r = api.RequestTextureVramFree();
    break;
  case Op::SetFogState:
    r = api.SetFogState(arg<remixapi_FogInfo>(A((ArgsInfo *)nullptr).info, S_FogInfo));
    break;
  case Op::SetScreenTint: {
    const auto &a = A((ArgsF4 *)nullptr);
    r = api.SetScreenTint(a.v[0], a.v[1], a.v[2], a.v[3]);
    break;
  }
  case Op::RegisterUITexture: {
    const auto &a = A((ArgsUITexture *)nullptr);
    r = api.RegisterUITexture(a.handle, a.width, a.height, static_cast<remixapi_Format>(a.format),
                              resolve(a.data), a.dataSize);
    break;
  }
  case Op::FreeUITexture:
    r = api.FreeUITexture(A((ArgsHandle *)nullptr).id);
    break;
  case Op::SubmitUIDrawList:
    r = api.SubmitUIDrawList(arg<remixapi_UIDrawList>(A((ArgsInfo *)nullptr).info, S_UIDrawList));
    break;
  case Op::RequestPresentedScreenshot:
    r = api.RequestPresentedScreenshot(static_cast<const char *>(resolve(A((ArgsStrings *)nullptr).a)));
    break;
  case Op::SetCameraMediumMaterial:
    r = api.SetCameraMediumMaterial(
        arg<remixapi_CameraMediumInfo>(A((ArgsInfo *)nullptr).info, S_CameraMediumInfo));
    break;
  case Op::PumpEvents:
    r = api.PumpEvents();
    break;
  case Op::GetWindowState: {
    remixapi_WindowState s = {};
    r = api.GetWindowState(&s);
    reply(seq, r, &s, sizeof(s));
    return;
  }
  case Op::PollMouseState: {
    remixapi_MouseState s = {};
    r = api.PollMouseState(&s);
    reply(seq, r, &s, sizeof(s));
    return;
  }
  case Op::IsKeyDown: {
    remixapi_Bool down = 0;
    r = api.IsKeyDown(static_cast<remixapi_Key>(A((ArgsU32 *)nullptr).value), &down);
    reply(seq, r, &down, sizeof(down));
    return;
  }
  case Op::SetMouseGrabbed:
    r = api.SetMouseGrabbed(A((ArgsU32 *)nullptr).value);
    break;
  case Op::SetCursorPosition: {
    const auto &a = A((ArgsF4 *)nullptr);
    r = api.SetCursorPosition(a.v[0], a.v[1]);
    break;
  }
  case Op::SetFullscreen:
    r = api.SetFullscreen(A((ArgsU32 *)nullptr).value);
    reply(seq, r);
    return;
  case Op::GetFeatureAvailability: {
    remixapi_FeatureAvailability f = {};
    r = api.GetFeatureAvailability(&f);
    reply(seq, r, &f, sizeof(f));
    return;
  }
  case Op::PollInputEvents: {
    const uint32_t capacity = A((ArgsU32 *)nullptr).value;
    uint32_t count = 0;
    r = m_pollInput ? m_pollInput(reinterpret_cast<remixapi_InputEvent *>(replyPayload()), capacity, &count)
                    : REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    reply(seq, r, replyPayload(), count * sizeof(remixapi_InputEvent));
    return;
  }
  case Op::SetTextInputEnabled:
    r = m_textInput ? m_textInput(A((ArgsU32 *)nullptr).value) : REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    break;
  case Op::GetWindowMetrics: {
    remixapi_WindowMetrics m = {};
    r = m_metrics ? m_metrics(&m) : REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    reply(seq, r, &m, sizeof(m));
    return;
  }
  case Op::GetPresentationCount: {
    uint64_t count = 0;
    r = m_presentations ? m_presentations(&count) : REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    reply(seq, r, &count, sizeof(count));
    return;
  }
  case Op::GetStatistics: {
    const auto &a = A((ArgsStat *)nullptr);
    const auto kind = static_cast<size_t>(a.kind);
    const uint32_t size = a.size < kReplySize - sizeof(Reply) ? a.size : kReplySize - sizeof(Reply);
    std::memset(replyPayload(), 0, size);
    std::memcpy(replyPayload(), &size, 4); // structSize
    r = kind < static_cast<size_t>(StatKind::Count) && m_stats[kind] ? m_stats[kind](replyPayload())
                                                                     : REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    reply(seq, r, replyPayload(), size);
    return;
  }
  case Op::ConfigureUSDMods: {
    const auto &a = A((ArgsUsdMods *)nullptr);
    remixapi_USDModsOptionsEXT options = {};
    options.structSize = sizeof(options);
    options.version = 1;
    options.directory = static_cast<const char *>(resolve(a.directory));
    options.hostMetersPerUnit = a.hostMetersPerUnit;
    options.hostUpAxis = a.hostUpAxis;
    options.watch = a.watch;
    r = m_configureMods ? m_configureMods(&options) : REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    g_log.write("host: ConfigureUSDMods(%s) -> %d", options.directory ? options.directory : "(null)", int(r));
    reply(seq, r);
    return;
  }
  case Op::StoreBatch: {
    const auto &a = A((ArgsBatch *)nullptr);
    const auto *data = static_cast<const uint8_t *>(resolve(a.data));
    if (!data) {
      asyncResult(op, REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS);
      return;
    }
    m_batches[a.id].assign(data, data + a.size);
    return;
  }
  case Op::ReplayBatch: {
    auto it = m_batches.find(A((ArgsHandle *)nullptr).id);
    if (it == m_batches.end()) {
      asyncResult(op, REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS);
      return;
    }
    // A fresh copy each time: decoding rewrites pointers and handles in place.
    m_replay.assign(it->second.begin(), it->second.end());
    const auto &batch = m_replay;
    for (size_t at = 0; at + sizeof(Record) <= batch.size();) {
      const auto *rec = reinterpret_cast<const Record *>(batch.data() + at);
      if (rec->size < sizeof(Record) || at + rec->size > batch.size() || rec->op == Op::ReplayBatch ||
          rec->op == Op::StoreBatch)
        break;
      execute(rec, rec->size);
      at += rec->size;
    }
    return;
  }
  case Op::DeleteBatch:
    m_batches.erase(A((ArgsHandle *)nullptr).id);
    return;
  case Op::SetVisibilityPartition: {
    const auto &a = A((ArgsVisibility *)nullptr);
    remixapi_VisibilityPartitionEXT partition = {};
    partition.structSize = sizeof(partition);
    partition.nodeCount = a.nodeCount;
    partition.planes = static_cast<const float *>(resolve(a.planes));
    partition.children = static_cast<const int32_t *>(resolve(a.children));
    partition.leafCount = a.leafCount;
    partition.leafClusters = static_cast<const int32_t *>(resolve(a.leafClusters));
    partition.clusterCount = a.clusterCount;
    partition.rowBytes = a.rowBytes;
    partition.visibility = static_cast<const uint8_t *>(resolve(a.visibility));
    r = m_setVisibility ? m_setVisibility(a.nodeCount ? &partition : nullptr) : REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    g_log.write("host: SetVisibilityPartition(%u nodes, %u leaves, %u clusters) -> %d", a.nodeCount, a.leafCount,
                a.clusterCount, int(r));
    reply(seq, r);
    return;
  }
  case Op::GetUSDModsDiagnostic: {
    char *out = reinterpret_cast<char *>(replyPayload());
    out[0] = 0;
    r = m_modsDiagnostic ? m_modsDiagnostic(out, 4096) : REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    reply(seq, r, out, uint32_t(strnlen(out, 4095)));
    return;
  }
  case Op::InjectInput:
    injectInput(A((ArgsInput *)nullptr));
    r = REMIXAPI_ERROR_CODE_SUCCESS;
    break;
  default:
    g_log.write("host: unknown op %u", static_cast<unsigned>(op));
    r = REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    break;
  }
  asyncResult(op, r);
  reply(seq, r);
}

int Host::run() {
  uint64_t read = m_header->readPos.load();
  bool holdingData = false;
  HANDLE waits[2] = {m_work, m_client};
  for (;;) {
    uint64_t write = m_header->writePos.load(std::memory_order_acquire);
    if (read == write) {
      if (m_header->clientClosing.load())
        return 0;
      bool woke = false;
      for (int spin = 0; spin < 2000 && !woke; ++spin) {
        YieldProcessor();
        woke = m_header->writePos.load(std::memory_order_acquire) != read;
      }
      if (woke)
        continue;
      m_header->hostSleeping.store(1);
      if (m_header->writePos.load() != read || m_header->clientClosing.load()) {
        m_header->hostSleeping.store(0);
        continue;
      }
      if (WaitForMultipleObjects(2, waits, FALSE, 100) == WAIT_OBJECT_0 + 1) {
        g_log.write("host: client process exited");
        return 0;
      }
      continue;
    }

    const auto *rec = reinterpret_cast<const Record *>(m_ring + read % m_ringSize);
    const uint32_t size = rec->size;
    if (size < sizeof(Record) || size % kRecordAlign || read % m_ringSize + size > m_ringSize) {
      snprintf(m_header->lastError, sizeof(m_header->lastError), "corrupt record at %llu (op %u size %u)",
               static_cast<unsigned long long>(read), static_cast<unsigned>(rec->op), size);
      g_log.write("host: %s", m_header->lastError);
      m_header->hostState.store(static_cast<uint32_t>(HostState::Failed));
      return 3;
    }

    switch (rec->op) {
    case Op::Pad:
      break;
    case Op::Data:
      holdingData = true; // released with the record that references it
      break;
    case Op::BlobBegin: {
      const auto *a = reinterpret_cast<const ArgsBlobBegin *>(rec + 1);
      m_blobs[a->id].resize(a->size);
      break;
    }
    case Op::BlobChunk: {
      const auto *a = reinterpret_cast<const ArgsBlobChunk *>(rec + 1);
      auto &blob = m_blobs[a->id];
      if (a->offset + a->size <= blob.size())
        std::memcpy(blob.data() + a->offset, a + 1, a->size);
      break;
    }
    default:
      if (g_trace)
        g_log.write("host: exec %s seq %llu size %u", opName(rec->op), static_cast<unsigned long long>(rec->seq), size);
      execute(rec, size);
      m_blobs.clear();
      holdingData = false;
      break;
    }
    read += size;
    if (!holdingData)
      release(read);
  }
}

} // namespace

// Logs the faulting stack as module+offset so host crashes inside the runtime
// can be located without a debugger.
LONG WINAPI crashFilter(EXCEPTION_POINTERS *e) {
  g_log.write("host: unhandled exception %08lx at %p (thread %lu)", e->ExceptionRecord->ExceptionCode,
              e->ExceptionRecord->ExceptionAddress, GetCurrentThreadId());
  void *frames[48];
  const USHORT count = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
  for (USHORT i = 0; i < count; ++i) {
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(frames[i]), &module))
      GetModuleFileNameA(module, name, MAX_PATH);
    const char *base = std::strrchr(name, '\\');
    g_log.write("  #%u %s+0x%llx", i, base ? base + 1 : name,
                static_cast<unsigned long long>(static_cast<char *>(frames[i]) - reinterpret_cast<char *>(module)));
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  SetUnhandledExceptionFilter(crashFilter);
  int argc = 0;
  wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  g_log.open(moduleDirectory(nullptr) + L"\\openremix_bridge_host.log");
  g_trace = GetEnvironmentVariableW(L"OPENREMIX_BRIDGE_TRACE", nullptr, 0) > 0;
  bridge::startProfilerWatch(moduleDirectory(nullptr));
  if (argc < 3) {
    g_log.write("host: usage: openremix_bridge_host <client pid> <nonce>");
    return 1;
  }
  const DWORD clientPid = wcstoul(argv[1], nullptr, 10);
  const DWORD nonce = wcstoul(argv[2], nullptr, 10);

  static Host host;
  if (!host.open(clientPid, nonce)) {
    g_log.write("host: failed to attach to client %lu", clientPid);
    return 1;
  }
  g_log.write("host: attached to client %lu", clientPid);
  // All runtime calls happen on this thread, which satisfies the runtime's
  // requirement that Startup, Present and Shutdown share a thread.
  if (!host.loadRuntime())
    return 2;
  const int code = host.run();
  g_log.write("host: exiting (%d)", code);
  return code;
}
