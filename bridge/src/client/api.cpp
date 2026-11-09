// x86 Remix API implementation that forwards every call to the x64 host.
//
// Create* calls are asynchronous: the client allocates handle ids and the
// host maps them to runtime handles, so a failed creation is reported in the
// host log (and its handle draws nothing) instead of through the return value.
// Getters wait for the host and return its result.
#define REMIX_ALLOW_X86
#define REMIX_LIBRARY_EXPORTS
#include <remix/remix_c.h>
#include <remix/remix_native_ext.h>
#include <remix/remix_usd_ext.h>

#include "client/encoder.h"
#include "client/transport.h"

#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

using namespace bridge;
using namespace bridge::client;
using namespace bridge::layout;
using proto::Op;

namespace {

class IdPool {
public:
  uint32_t alloc() {
    if (!m_free.empty()) {
      uint32_t id = m_free.back();
      m_free.pop_back();
      return id;
    }
    return m_next++;
  }
  // Ids are reused only after their Destroy command, which the host
  // executes in order, so a recycled id can never alias a live handle.
  void release(uint32_t id) {
    if (id)
      m_free.push_back(id);
  }

private:
  std::vector<uint32_t> m_free;
  uint32_t m_next = 1;
};

struct ClientState {
  SRWLOCK lock = SRWLOCK_INIT;
  Log log;
  Transport transport;
  Encoder encoder{transport, log};
  IdPool materials, meshes, lights, textures, sceneInstances, hostShaders;
  // Mesh ids by the hash they were created with: a refresh in place
  // (remixapi_MeshInfoDeformEXT) of a mesh created here keeps its id, as the
  // runtime keeps its handle, instead of taking (and never releasing) a new one.
  std::unordered_map<uint64_t, uint32_t> meshByHash;
  std::unordered_map<uint32_t, uint64_t> hashOfMesh;
  bool started = false;
  bool runtimeStarted = false;
  uint64_t framesSubmitted = 0;
  uint32_t maxQueuedFrames = 2;
  PFN_remixapi_BridgeCallback beginScene = nullptr, endScene = nullptr, present = nullptr;
  // openremix_bridge_BeginRecording: asynchronous records are also kept here.
  bool recording = false, recordingFailed = false;
  std::vector<uint8_t> recorded;
};

ClientState &state() {
  static ClientState *s = new ClientState(); // intentionally leaked: used during DLL detach
  return *s;
}

struct Lock {
  Lock() { AcquireSRWLockExclusive(&state().lock); }
  ~Lock() { ReleaseSRWLockExclusive(&state().lock); }
};

template <class T> T fromId(uint32_t id) { return reinterpret_cast<T>(static_cast<uintptr_t>(id)); }
template <class T> uint32_t toId(T handle) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(handle)); }

bool ready() { return state().started && state().transport.running(); }

Encoder &begin(Op op, uint32_t argsSize = 0) {
  state().encoder.reset(op, argsSize);
  return state().encoder;
}

remixapi_ErrorCode post(Encoder &e) {
  auto &s = state();
  // Only self-contained records replay: none referencing ring data or blobs
  // sent ahead of them (large payloads).
  // Only draws are recorded: whatever else (uploads, possibly from other
  // threads) happens meanwhile must not run again on every replay.
  const auto op = reinterpret_cast<const proto::Record *>(e.buffer().data())->op;
  const bool record = s.recording && (op == Op::DrawInstance || op == Op::DrawLightInstance) &&
                      !(s.recordingFailed |= s.transport.pendingData());
  s.transport.submit(e.buffer(), false);
  if (record) {
    // submit() aligned the record and set its size; asynchronous: seq 0.
    s.recorded.insert(s.recorded.end(), e.buffer().begin(), e.buffer().end());
  }
  return state().transport.running() ? REMIXAPI_ERROR_CODE_SUCCESS : REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
}

// Sends a synchronous command and copies up to `outSize` reply bytes.
remixapi_ErrorCode call(Encoder &e, void *out = nullptr, size_t outSize = 0, uint32_t *outCopied = nullptr) {
  const uint64_t seq = state().transport.submit(e.buffer(), true);
  const proto::Reply *reply = nullptr;
  const uint8_t *payload = nullptr;
  if (!state().transport.waitReply(seq, reply, payload))
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  const size_t n = reply->size < outSize ? reply->size : outSize;
  if (out && n)
    std::memcpy(out, payload, n);
  if (outCopied)
    *outCopied = static_cast<uint32_t>(n);
  return static_cast<remixapi_ErrorCode>(reply->result);
}

remixapi_ErrorCode postInfo(Op op, StructId id, const void *info) {
  Encoder &e = begin(op, sizeof(proto::ArgsInfo));
  e.setArgs(proto::ArgsInfo{e.structure(id, info)});
  return post(e);
}

remixapi_ErrorCode postHandle(Op op, uint32_t id) {
  Encoder &e = begin(op, sizeof(proto::ArgsHandle));
  e.setArgs(proto::ArgsHandle{id});
  return post(e);
}

remixapi_ErrorCode postStrings(Op op, const char *a, const char *b) {
  Encoder &e = begin(op, sizeof(proto::ArgsStrings));
  const uint64_t pa = e.cstr(a);
  const uint64_t pb = e.cstr(b);
  e.setArgs(proto::ArgsStrings{pa, pb});
  return post(e);
}

remixapi_ErrorCode postU32(Op op, uint32_t value) {
  Encoder &e = begin(op, sizeof(proto::ArgsU32));
  e.setArgs(proto::ArgsU32{value, 0});
  return post(e);
}

// Bytes of a tightly packed image, or 0 for formats the bridge does not know.
uint64_t imageBytes(remixapi_Format format, uint32_t w, uint32_t h) {
  const uint64_t bw = (w + 3) / 4, bh = (h + 3) / 4;
  switch (static_cast<uint32_t>(format)) {
  case REMIXAPI_FORMAT_R8G8B8A8_UNORM:
  case REMIXAPI_FORMAT_R8G8B8A8_SRGB:
  case REMIXAPI_FORMAT_B8G8R8A8_UNORM:
  case REMIXAPI_FORMAT_B8G8R8A8_SRGB:
    return uint64_t(w) * h * 4;
  case REMIXAPI_FORMAT_BC1_RGB_UNORM:
  case REMIXAPI_FORMAT_BC1_RGB_SRGB:
  case 133: // BC1_RGBA_UNORM_EXT
  case 134: // BC1_RGBA_SRGB_EXT
  case 147: // BC4_UNORM_EXT
    return bw * bh * 8;
  case REMIXAPI_FORMAT_BC3_UNORM:
  case REMIXAPI_FORMAT_BC3_SRGB:
  case REMIXAPI_FORMAT_BC5_UNORM:
  case REMIXAPI_FORMAT_BC7_UNORM:
  case REMIXAPI_FORMAT_BC7_SRGB:
  case 143: // BC6H_UFLOAT_EXT
  case 144: // BC6H_SFLOAT_EXT
    return bw * bh * 16;
  default:
    return 0;
  }
}

#define GUARD()                                                                                                \
  Lock guard;                                                                                                  \
  if (!ready())                                                                                                \
  return REMIXAPI_ERROR_CODE_NOT_INITIALIZED

// GUARD() for an optional runtime export (proto::RuntimeExport).
#define GUARD_EXPORT(bit)                                                                                      \
  GUARD();                                                                                                     \
  if (!(state().transport.header()->runtimeExports & proto::bit))                                             \
  return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE

} // namespace

extern "C" {

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_Startup(const remixapi_StartupInfo *info) {
  if (!info || info->sType != REMIXAPI_STRUCT_TYPE_STARTUP_INFO)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD();
  Encoder &e = begin(Op::Startup, sizeof(proto::ArgsInfo));
  e.setArgs(proto::ArgsInfo{e.structure(S_StartupInfo, info)});
  const auto result = call(e);
  state().runtimeStarted = result == REMIXAPI_ERROR_CODE_SUCCESS;
  state().log.write("bridge: Startup(hwnd=%p) -> %d", info->hwnd, int(result));
  return result;
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_Shutdown(void) {
  GUARD();
  const auto result = call(begin(Op::Shutdown));
  state().runtimeStarted = false;
  return result;
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_Present(const remixapi_PresentInfo *info) {
  PFN_remixapi_BridgeCallback endScene, present, beginScene;
  {
    Lock guard;
    endScene = state().endScene;
    present = state().present;
    beginScene = state().beginScene;
  }
  if (endScene)
    endScene();
  if (present)
    present();
  remixapi_ErrorCode result;
  {
    GUARD();
    auto &s = state();
    s.transport.throttleFrames(s.framesSubmitted, s.maxQueuedFrames);
    Encoder &e = begin(Op::Present, sizeof(proto::ArgsInfo));
    e.setArgs(proto::ArgsInfo{e.structure(S_PresentInfo, info)});
    result = post(e);
    ++s.framesSubmitted;
  }
  if (beginScene)
    beginScene();
  return result;
}

static remixapi_ErrorCode REMIXAPI_CALL createMaterial(const remixapi_MaterialInfo *info,
                                                       remixapi_MaterialHandle *out) {
  if (!info || !out || info->sType != REMIXAPI_STRUCT_TYPE_MATERIAL_INFO)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD();
  const uint32_t id = state().materials.alloc();
  Encoder &e = begin(Op::CreateMaterial, sizeof(proto::ArgsCreate));
  e.setArgs(proto::ArgsCreate{id, e.structure(S_MaterialInfo, info)});
  *out = fromId<remixapi_MaterialHandle>(id);
  return post(e);
}

static remixapi_ErrorCode REMIXAPI_CALL destroyMaterial(remixapi_MaterialHandle handle) {
  if (!handle)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD();
  auto r = postHandle(Op::DestroyMaterial, toId(handle));
  state().materials.release(toId(handle));
  return r;
}

static remixapi_ErrorCode REMIXAPI_CALL createMesh(const remixapi_MeshInfo *info, remixapi_MeshHandle *out) {
  if (!info || !out || info->sType != REMIXAPI_STRUCT_TYPE_MESH_INFO)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD();
  auto &s = state();
  struct Header { // every extension struct starts so
    remixapi_StructType sType;
    const void *pNext;
  };
  bool deform = false;
  for (auto *next = static_cast<const Header *>(info->pNext); next && !deform;
       next = static_cast<const Header *>(next->pNext))
    deform = next->sType == REMIXAPI_STRUCT_TYPE_MESH_DEFORM_EXT;
  const auto known = s.meshByHash.find(info->hash);
  uint32_t id;
  if (deform && known != s.meshByHash.end()) {
    id = known->second;
  } else {
    id = s.meshes.alloc();
    if (known != s.meshByHash.end())
      s.hashOfMesh.erase(known->second); // that id keeps its handle, no longer this hash
    s.meshByHash[info->hash] = id;
    s.hashOfMesh[id] = info->hash;
  }
  Encoder &e = begin(Op::CreateMesh, sizeof(proto::ArgsCreate));
  e.setArgs(proto::ArgsCreate{id, e.structure(S_MeshInfo, info)});
  *out = fromId<remixapi_MeshHandle>(id);
  return post(e);
}

static remixapi_ErrorCode REMIXAPI_CALL destroyMesh(remixapi_MeshHandle handle) {
  if (!handle)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD();
  auto r = postHandle(Op::DestroyMesh, toId(handle));
  auto &s = state();
  const auto hash = s.hashOfMesh.find(toId(handle));
  if (hash != s.hashOfMesh.end()) {
    s.meshByHash.erase(hash->second);
    s.hashOfMesh.erase(hash);
  }
  s.meshes.release(toId(handle));
  return r;
}

static remixapi_ErrorCode REMIXAPI_CALL setupCamera(const remixapi_CameraInfo *info) {
  if (!info || info->sType != REMIXAPI_STRUCT_TYPE_CAMERA_INFO)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD();
  return postInfo(Op::SetupCamera, S_CameraInfo, info);
}

static remixapi_ErrorCode REMIXAPI_CALL drawInstance(const remixapi_InstanceInfo *info) {
  if (!info || info->sType != REMIXAPI_STRUCT_TYPE_INSTANCE_INFO)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD();
  return postInfo(Op::DrawInstance, S_InstanceInfo, info);
}

static remixapi_ErrorCode REMIXAPI_CALL createLight(const remixapi_LightInfo *info, remixapi_LightHandle *out) {
  if (!info || !out || info->sType != REMIXAPI_STRUCT_TYPE_LIGHT_INFO)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD();
  const uint32_t id = state().lights.alloc();
  Encoder &e = begin(Op::CreateLight, sizeof(proto::ArgsCreate));
  e.setArgs(proto::ArgsCreate{id, e.structure(S_LightInfo, info)});
  *out = fromId<remixapi_LightHandle>(id);
  return post(e);
}

static remixapi_ErrorCode REMIXAPI_CALL destroyLight(remixapi_LightHandle handle) {
  if (!handle)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD();
  auto r = postHandle(Op::DestroyLight, toId(handle));
  state().lights.release(toId(handle));
  return r;
}

static remixapi_ErrorCode REMIXAPI_CALL drawLightInstance(remixapi_LightHandle handle) {
  if (!handle)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD();
  return postHandle(Op::DrawLightInstance, toId(handle));
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_UpdateLightDefinition(remixapi_LightHandle handle,
                                                                        const remixapi_LightInfo *info) {
  if (!handle || !info || info->sType != REMIXAPI_STRUCT_TYPE_LIGHT_INFO)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD();
  Encoder &e = begin(Op::UpdateLightDefinition, sizeof(proto::ArgsLightUpdate));
  e.setArgs(proto::ArgsLightUpdate{toId(handle), e.structure(S_LightInfo, info)});
  return post(e);
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_AutoInstancePersistentLights(void) {
  GUARD();
  return post(begin(Op::AutoInstancePersistentLights));
}

static remixapi_ErrorCode REMIXAPI_CALL setConfigVariable(const char *key, const char *value) {
  if (!key || !value)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD();
  // Synchronous: callers probe for FEATURE_UNAVAILABLE (e.g. selecting a backend).
  Encoder &e = begin(Op::SetConfigVariable, sizeof(proto::ArgsStrings));
  const uint64_t pk = e.cstr(key);
  const uint64_t pv = e.cstr(value);
  e.setArgs(proto::ArgsStrings{pk, pv});
  return call(e);
}

static remixapi_ErrorCode REMIXAPI_CALL addTextureHash(const char *category, const char *hash) {
  GUARD();
  return postStrings(Op::AddTextureHash, category, hash);
}

static remixapi_ErrorCode REMIXAPI_CALL removeTextureHash(const char *category, const char *hash) {
  GUARD();
  return postStrings(Op::RemoveTextureHash, category, hash);
}

static remixapi_ErrorCode REMIXAPI_CALL createTexture(const remixapi_TextureInfo *info,
                                                      remixapi_TextureHandle *out) {
  if (!info || !out || info->sType != REMIXAPI_STRUCT_TYPE_TEXTURE_INFO)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD();
  const uint32_t id = state().textures.alloc();
  Encoder &e = begin(Op::CreateTexture, sizeof(proto::ArgsCreate));
  e.setArgs(proto::ArgsCreate{id, e.structure(S_TextureInfo, info)});
  *out = fromId<remixapi_TextureHandle>(id);
  return post(e);
}

static remixapi_ErrorCode REMIXAPI_CALL destroyTexture(remixapi_TextureHandle handle) {
  if (!handle)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD();
  auto r = postHandle(Op::DestroyTexture, toId(handle));
  state().textures.release(toId(handle));
  return r;
}

REMIXAPI remixapi_UIState REMIXAPI_CALL remixapi_GetUIState(void) {
  Lock guard;
  if (!ready())
    return REMIXAPI_UI_STATE_NONE;
  uint32_t value = REMIXAPI_UI_STATE_NONE;
  call(begin(Op::GetUIState), &value, sizeof(value));
  return static_cast<remixapi_UIState>(value);
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetUIState(remixapi_UIState ui) {
  GUARD();
  return postU32(Op::SetUIState, static_cast<uint32_t>(ui));
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_RegisterCallbacks(PFN_remixapi_BridgeCallback beginScene,
                                                                    PFN_remixapi_BridgeCallback endScene,
                                                                    PFN_remixapi_BridgeCallback present) {
  Lock guard;
  state().beginScene = beginScene;
  state().endScene = endScene;
  state().present = present;
  return REMIXAPI_ERROR_CODE_SUCCESS;
}

static remixapi_ErrorCode REMIXAPI_CALL drawScreenOverlay(const void *pixels, uint32_t width, uint32_t height,
                                                          remixapi_Format format, float opacity) {
  const uint64_t size = imageBytes(format, width, height);
  if (!pixels || !size)
    return pixels ? REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE : REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD();
  Encoder &e = begin(Op::DrawScreenOverlay, sizeof(proto::ArgsOverlay));
  const uint64_t p = e.bytes(pixels, size);
  e.setArgs(proto::ArgsOverlay{p, width, height, static_cast<uint32_t>(format), opacity});
  return post(e);
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetGameValue(const char *key, const char *value) {
  GUARD();
  return postStrings(Op::SetGameValue, key, value);
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetGameValue(const char *key, char *out, uint32_t capacity,
                                                               uint32_t *outLength) {
  if (!key)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD();
  Encoder &e = begin(Op::GetGameValue, sizeof(proto::ArgsGameValue));
  const uint64_t pk = e.cstr(key);
  e.setArgs(proto::ArgsGameValue{pk, capacity, 0});
  // Reply: uint32_t length, then the value (NUL-terminated, truncated to capacity).
  uint8_t reply[4 + 4096] = {};
  uint32_t copied = 0;
  const auto result = call(e, reply, sizeof(reply), &copied);
  uint32_t length = 0;
  std::memcpy(&length, reply, 4);
  if (outLength)
    *outLength = length;
  if (out && capacity && copied > 4) {
    const uint32_t n = copied - 4 < capacity ? copied - 4 : capacity;
    std::memcpy(out, reply + 4, n);
    out[capacity - 1 < n ? capacity - 1 : n] = '\0';
  }
  return result;
}

static remixapi_ErrorCode REMIXAPI_CALL requestVramCompaction(void) {
  GUARD();
  return post(begin(Op::RequestVramCompaction));
}

static remixapi_ErrorCode REMIXAPI_CALL getVramStats(remixapi_VramStats *out) {
  if (!out)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD();
  return call(begin(Op::GetVramStats), out, sizeof(*out));
}

static remixapi_ErrorCode REMIXAPI_CALL requestTextureVramFree(void) {
  GUARD();
  return post(begin(Op::RequestTextureVramFree));
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetFogState(const remixapi_FogInfo *info) {
  if (!info)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD();
  return postInfo(Op::SetFogState, S_FogInfo, info);
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetScreenTint(float r, float g, float b, float a) {
  GUARD();
  Encoder &e = begin(Op::SetScreenTint, sizeof(proto::ArgsF4));
  e.setArgs(proto::ArgsF4{{r, g, b, a}});
  return post(e);
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_RegisterUITexture(remixapi_UITextureHandle handle, uint32_t width,
                                                                    uint32_t height, remixapi_Format format,
                                                                    const void *data, uint64_t dataSize) {
  GUARD();
  Encoder &e = begin(Op::RegisterUITexture, sizeof(proto::ArgsUITexture));
  const uint64_t p = e.bytes(data, static_cast<size_t>(dataSize));
  e.setArgs(proto::ArgsUITexture{handle, p, dataSize, width, height, static_cast<uint32_t>(format), 0});
  return post(e);
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_FreeUITexture(remixapi_UITextureHandle handle) {
  GUARD();
  Encoder &e = begin(Op::FreeUITexture, sizeof(proto::ArgsHandle));
  e.setArgs(proto::ArgsHandle{handle});
  return post(e);
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SubmitUIDrawList(const remixapi_UIDrawList *list) {
  if (!list)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD();
  return postInfo(Op::SubmitUIDrawList, S_UIDrawList, list);
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_RequestPresentedScreenshot(const char *path) {
  if (!path)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD();
  return postStrings(Op::RequestPresentedScreenshot, path, nullptr);
}

static remixapi_ErrorCode REMIXAPI_CALL setCameraMediumMaterial(const remixapi_CameraMediumInfo *info) {
  GUARD();
  return postInfo(Op::SetCameraMediumMaterial, S_CameraMediumInfo, info);
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_PumpEvents(void) {
  GUARD();
  return post(begin(Op::PumpEvents));
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetWindowState(remixapi_WindowState *out) {
  if (!out)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD();
  return call(begin(Op::GetWindowState), out, sizeof(*out));
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_PollMouseState(remixapi_MouseState *out) {
  if (!out)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD();
  return call(begin(Op::PollMouseState), out, sizeof(*out));
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_IsKeyDown(remixapi_Key key, remixapi_Bool *out) {
  if (!out)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD();
  Encoder &e = begin(Op::IsKeyDown, sizeof(proto::ArgsU32));
  e.setArgs(proto::ArgsU32{static_cast<uint32_t>(key), 0});
  return call(e, out, sizeof(*out));
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetMouseGrabbed(remixapi_Bool grabbed) {
  GUARD();
  return postU32(Op::SetMouseGrabbed, grabbed);
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetCursorPosition(float x, float y) {
  GUARD();
  Encoder &e = begin(Op::SetCursorPosition, sizeof(proto::ArgsF4));
  e.setArgs(proto::ArgsF4{{x, y, 0, 0}});
  return post(e);
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetFullscreen(remixapi_Bool fullscreen) {
  GUARD();
  return call([&]() -> Encoder & {
    Encoder &e = begin(Op::SetFullscreen, sizeof(proto::ArgsU32));
    e.setArgs(proto::ArgsU32{static_cast<uint32_t>(fullscreen), 0});
    return e;
  }());
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetFeatureAvailability(remixapi_FeatureAvailability *out) {
  if (!out)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD();
  return call(begin(Op::GetFeatureAvailability), out, sizeof(*out));
}

// Native extensions (remix_native_ext.h).
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_PollInputEvents(remixapi_InputEvent *events, uint32_t capacity,
                                                                  uint32_t *outCount) {
  if (!outCount || (!events && capacity))
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD();
  const uint32_t limit = static_cast<uint32_t>((proto::kReplySize - sizeof(proto::Reply)) / sizeof(*events));
  Encoder &e = begin(Op::PollInputEvents, sizeof(proto::ArgsU32));
  e.setArgs(proto::ArgsU32{capacity < limit ? capacity : limit, 0});
  uint32_t copied = 0;
  const auto result = call(e, events, size_t(capacity) * sizeof(*events), &copied);
  *outCount = copied / sizeof(*events);
  return result;
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetTextInputEnabled(remixapi_Bool enabled) {
  GUARD();
  return postU32(Op::SetTextInputEnabled, enabled);
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetWindowMetrics(remixapi_WindowMetrics *out) {
  if (!out)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD();
  return call(begin(Op::GetWindowMetrics), out, sizeof(*out));
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetPresentationCount(uint64_t *out) {
  if (!out)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD();
  return call(begin(Op::GetPresentationCount), out, sizeof(*out));
}

static remixapi_ErrorCode getStatistics(proto::StatKind kind, void *out, uint32_t size) {
  if (!out)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  uint32_t structSize = 0;
  std::memcpy(&structSize, out, 4);
  if (structSize && structSize < size)
    size = structSize;
  GUARD();
  Encoder &e = begin(Op::GetStatistics, sizeof(proto::ArgsStat));
  e.setArgs(proto::ArgsStat{kind, size});
  return call(e, out, size);
}

#define STAT_GETTER(fn, type, kind)                                                                            \
  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_##fn(type *out) {                                         \
    return getStatistics(proto::StatKind::kind, out, sizeof(type));                                            \
  }
STAT_GETTER(GetBackendCapabilitiesEXT, remixapi_BackendCapabilitiesEXT, BackendCapabilities)
STAT_GETTER(GetLocalLightStatisticsEXT, remixapi_LocalLightStatisticsEXT, LocalLight)
STAT_GETTER(GetLocalLightGuidingStatisticsEXT, remixapi_LocalLightGuidingStatisticsEXT, LocalLightGuiding)
STAT_GETTER(GetShadowStatisticsEXT, remixapi_ShadowStatisticsEXT, Shadow)
STAT_GETTER(GetSharcStatisticsEXT, remixapi_SharcStatisticsEXT, Sharc)
STAT_GETTER(GetTracingStatisticsEXT, remixapi_TracingStatisticsEXT, Tracing)
STAT_GETTER(GetFramePerformanceEXT, remixapi_FramePerformanceEXT, FramePerformance)
STAT_GETTER(GetFrameBreakdownEXT, remixapi_FrameBreakdownEXT, FrameBreakdown)
STAT_GETTER(GetGpuPassTimingsEXT, remixapi_GpuPassTimingsEXT, GpuPassTimings)
STAT_GETTER(GetDenoisingCapabilitiesEXT, remixapi_DenoisingCapabilitiesEXT, DenoisingCapabilities)
STAT_GETTER(GetDenoisingStatisticsEXT, remixapi_DenoisingStatisticsEXT, Denoising)
STAT_GETTER(GetTextureStatisticsEXT, remixapi_TextureStatisticsEXT, Texture)
STAT_GETTER(GetCameraCullingStatisticsEXT, remixapi_CameraCullingStatisticsEXT, CameraCulling)
STAT_GETTER(GetLocalLightSetStatisticsEXT, remixapi_LocalLightSetStatisticsEXT, LocalLightSet)
#undef STAT_GETTER

// The statistics structs travel as raw bytes: their layout must not differ.
#define SAME_LAYOUT(type)                                                                                      \
  static_assert(kStructs[S_##type].size32 == kStructs[S_##type].size64 && sizeof(remixapi_##type) ==          \
                    kStructs[S_##type].size32,                                                                 \
                #type " differs between the ABIs")
SAME_LAYOUT(BackendCapabilitiesEXT);
SAME_LAYOUT(LocalLightStatisticsEXT);
SAME_LAYOUT(LocalLightGuidingStatisticsEXT);
SAME_LAYOUT(ShadowStatisticsEXT);
SAME_LAYOUT(SharcStatisticsEXT);
SAME_LAYOUT(TracingStatisticsEXT);
SAME_LAYOUT(FramePerformanceEXT);
SAME_LAYOUT(FrameBreakdownEXT);
SAME_LAYOUT(GpuPassTimingsEXT);
SAME_LAYOUT(DenoisingCapabilitiesEXT);
SAME_LAYOUT(DenoisingStatisticsEXT);
SAME_LAYOUT(TextureStatisticsEXT);
SAME_LAYOUT(CameraCullingStatisticsEXT);
SAME_LAYOUT(LocalLightSetStatisticsEXT);
SAME_LAYOUT(HostPassStatisticsEXT);
SAME_LAYOUT(SceneInstanceChangeEXT);
#undef SAME_LAYOUT
static_assert(sizeof(remixapi_HostPassStatisticsEXT) <= sizeof(proto::Header::hostPassStats));

// Settings defaults: synchronous like SetConfigVariable (a host falls back to
// SetConfigVariable on failure; called a few times at startup).
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetConfigDefaultEXT(const char *key, const char *value) {
  if (!key || !value)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD_EXPORT(kExportConfigDefault);
  Encoder &e = begin(Op::SetConfigDefault, sizeof(proto::ArgsStrings));
  const uint64_t pk = e.cstr(key);
  const uint64_t pv = e.cstr(value);
  e.setArgs(proto::ArgsStrings{pk, pv});
  return call(e);
}

// Region updates are asynchronous (per frame): SUCCESS means queued, the
// host logs refusals. Rows go tightly packed whatever rowPitch is.
static remixapi_ErrorCode postRegion(Op op, proto::RuntimeExport bit, uint64_t handle, uint32_t x, uint32_t y,
                                     uint32_t width, uint32_t height, remixapi_Format format, const void *pixels,
                                     uint32_t rowPitch, uint32_t texelBytes) {
  const uint64_t rowBytes = uint64_t(width) * texelBytes;
  if (!handle || !pixels || !width || !height || rowPitch < rowBytes)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD();
  if (!(state().transport.header()->runtimeExports & bit))
    return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
  Encoder &e = begin(op, sizeof(proto::ArgsRegion));
  const uint64_t p = e.rows(pixels, size_t(rowBytes), height, rowPitch);
  e.setArgs(proto::ArgsRegion{handle, p, x, y, width, height, static_cast<uint32_t>(format),
                              static_cast<uint32_t>(rowBytes)});
  return post(e);
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_UpdateTextureEXT(remixapi_TextureHandle handle, uint32_t x,
                                                                   uint32_t y, uint32_t width, uint32_t height,
                                                                   remixapi_Format format, const void *pixels,
                                                                   uint32_t rowPitch) {
  uint32_t texel;
  switch (static_cast<uint32_t>(format)) {
  case REMIXAPI_FORMAT_R16G16B16A16_SFLOAT_EXT:
    texel = 8;
    break;
  case REMIXAPI_FORMAT_R32G32B32A32_SFLOAT_EXT:
    texel = 16;
    break;
  case REMIXAPI_FORMAT_R8G8B8A8_UNORM:
  case REMIXAPI_FORMAT_R8G8B8A8_SRGB:
  case REMIXAPI_FORMAT_B8G8R8A8_UNORM:
  case REMIXAPI_FORMAT_B8G8R8A8_SRGB:
    texel = 4;
    break;
  default:
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  }
  return postRegion(Op::UpdateTexture, proto::kExportUpdateTexture, toId(handle), x, y, width, height, format, pixels,
                    rowPitch, texel);
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_UpdateUITextureRegionEXT(remixapi_UITextureHandle id, uint32_t x,
                                                                            uint32_t y, uint32_t width, uint32_t height,
                                                                            remixapi_Format format, const void *pixels,
                                                                            uint32_t rowPitch) {
  return postRegion(Op::UpdateUITextureRegion, proto::kExportUpdateUITextureRegion, id, x, y, width, height, format,
                    pixels, rowPitch, 4);
}

// Scene instances. Handles are client ids (like Create*): AddSceneInstancesEXT
// returns SUCCESS once queued and the host maps the ids to the runtime's
// handles. A batch the runtime refuses adds nothing (the host logs it, its ids
// draw nothing and later calls on them are dropped).
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_AddSceneInstancesEXT(const remixapi_InstanceInfo *infos,
                                                                       uint32_t count, uint64_t *outInstances) {
  if (!count)
    return REMIXAPI_ERROR_CODE_SUCCESS;
  if (!infos || !outInstances)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  for (uint32_t i = 0; i < count; ++i)
    if (infos[i].sType != REMIXAPI_STRUCT_TYPE_INSTANCE_INFO || !infos[i].mesh)
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD_EXPORT(kExportSceneInstances);
  std::vector<uint64_t> ids(count);
  for (auto &id : ids)
    id = state().sceneInstances.alloc();
  Encoder &e = begin(Op::AddSceneInstances, sizeof(proto::ArgsSceneAdd));
  const uint64_t pids = e.bytes(ids.data(), ids.size() * sizeof(uint64_t), 8);
  const uint64_t pinfos = e.structures(S_InstanceInfo, infos, count);
  e.setArgs(proto::ArgsSceneAdd{pids, pinfos, count, 0});
  std::memcpy(outInstances, ids.data(), ids.size() * sizeof(uint64_t));
  return post(e);
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetSceneInstanceEXT(uint64_t instance,
                                                                      const remixapi_InstanceInfo *info) {
  if (!instance || !info || info->sType != REMIXAPI_STRUCT_TYPE_INSTANCE_INFO || !info->mesh)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD_EXPORT(kExportSceneInstances);
  Encoder &e = begin(Op::SetSceneInstance, sizeof(proto::ArgsCreate));
  e.setArgs(proto::ArgsCreate{instance, e.structure(S_InstanceInfo, info)});
  return post(e);
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_ChangeSceneInstancesEXT(const remixapi_SceneInstanceChangeEXT *changes, uint32_t count) {
  if (!count)
    return REMIXAPI_ERROR_CODE_SUCCESS;
  if (!changes)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD_EXPORT(kExportSceneInstances);
  // Identical layout on both ABIs; the host rewrites `instance` in place.
  Encoder &e = begin(Op::ChangeSceneInstances, sizeof(proto::ArgsArray));
  const uint64_t p = e.bytes(changes, size_t(count) * sizeof(*changes));
  e.setArgs(proto::ArgsArray{p, count, 0});
  return post(e);
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_RemoveSceneInstancesEXT(const uint64_t *instances, uint32_t count) {
  if (!count)
    return REMIXAPI_ERROR_CODE_SUCCESS;
  if (!instances)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD_EXPORT(kExportSceneInstances);
  Encoder &e = begin(Op::RemoveSceneInstances, sizeof(proto::ArgsArray));
  const uint64_t p = e.bytes(instances, size_t(count) * sizeof(*instances), 8);
  e.setArgs(proto::ArgsArray{p, count, 0});
  const auto r = post(e);
  for (uint32_t i = 0; i < count; ++i)
    state().sceneInstances.release(static_cast<uint32_t>(instances[i]));
  return r;
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_DrawSceneInstancesEXT(const float *origin) {
  GUARD_EXPORT(kExportSceneInstances);
  Encoder &e = begin(Op::DrawSceneInstances, sizeof(proto::ArgsOrigin));
  proto::ArgsOrigin args = {origin != nullptr, {}};
  if (origin)
    std::memcpy(args.origin, origin, sizeof(args.origin));
  e.setArgs(args);
  return post(e);
}

// Host passes. Shader handles are client ids: RegisterHostShaderEXT returns
// SUCCESS once queued (a shader the runtime refuses is logged by the host and
// its passes are skipped by the runtime as without a shader). Pass lists and
// target updates are asynchronous; host target handles are the game's values.
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_RegisterHostShaderEXT(const remixapi_HostShaderInfoEXT *info,
                                                                        remixapi_HostShaderHandleEXT *out) {
  if (!info || !out || info->sType != REMIXAPI_STRUCT_TYPE_HOST_SHADER_INFO_EXT || !info->code || !info->codeBytes ||
      info->codeBytes % 4 || (info->bindingCount && !info->bindings))
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD_EXPORT(kExportHostPasses);
  const uint32_t id = state().hostShaders.alloc();
  Encoder &e = begin(Op::RegisterHostShader, sizeof(proto::ArgsCreate));
  e.setArgs(proto::ArgsCreate{id, e.structure(S_HostShaderInfoEXT, info)});
  *out = id;
  return post(e);
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_DestroyHostShaderEXT(remixapi_HostShaderHandleEXT shader) {
  if (!shader)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD_EXPORT(kExportHostPasses);
  auto r = postHandle(Op::DestroyHostShader, shader);
  state().hostShaders.release(static_cast<uint32_t>(shader));
  return r;
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SubmitHostPassesEXT(const remixapi_HostPassListEXT *list) {
  if (list && list->sType != REMIXAPI_STRUCT_TYPE_HOST_PASS_LIST_EXT)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD_EXPORT(kExportHostPasses);
  return postInfo(Op::SubmitHostPasses, S_HostPassListEXT, list);
}

static remixapi_ErrorCode postHostImage(Op op, proto::RuntimeExport bit, remixapi_HostTargetHandleEXT target,
                                        uint32_t width, uint32_t height, uint32_t depth, uint32_t vkFormat,
                                        const void *pixels, uint64_t bytes) {
  if (!pixels || !bytes || bytes > 0x7fffffffu)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD();
  if (!(state().transport.header()->runtimeExports & bit))
    return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
  Encoder &e = begin(op, sizeof(proto::ArgsHostImage));
  const uint64_t p = e.bytes(pixels, static_cast<size_t>(bytes));
  e.setArgs(proto::ArgsHostImage{target, p, bytes, width, height, depth, vkFormat});
  return post(e);
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_UpdateHostTargetEXT(remixapi_HostTargetHandleEXT target,
                                                                       uint32_t width, uint32_t height,
                                                                       uint32_t vkFormat, const void *pixels,
                                                                       uint64_t bytes) {
  return postHostImage(Op::UpdateHostTarget, proto::kExportHostPasses, target, width, height, 0, vkFormat, pixels,
                       bytes);
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_UpdateHostVolumeEXT(remixapi_HostTargetHandleEXT target,
                                                                       uint32_t width, uint32_t height, uint32_t depth,
                                                                       uint32_t vkFormat, const void *pixels,
                                                                       uint64_t bytes) {
  return postHostImage(Op::UpdateHostVolume, proto::kExportHostVolume, target, width, height, depth, vkFormat, pixels,
                       bytes);
}

// Per-frame getters answered from the shared header (see proto::Header): the
// value as of the last Present the host executed, never a round trip.
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetHostPassesEnabledEXT(remixapi_Bool *enabled) {
  if (!enabled)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD();
  const uint32_t v = state().transport.header()->hostPassesEnabled.load(std::memory_order_acquire);
  if (v == proto::kHostPassesUnavailable)
    return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
  *enabled = v;
  return REMIXAPI_ERROR_CODE_SUCCESS;
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetHostPassStatisticsEXT(remixapi_HostPassStatisticsEXT *out) {
  if (!out)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  const uint64_t requested = out->structSize;
  const size_t size = requested && requested < sizeof(*out) ? static_cast<size_t>(requested) : sizeof(*out);
  GUARD();
  const proto::Header *h = state().transport.header();
  alignas(8) uint8_t copy[sizeof(*out)];
  int32_t result;
  for (;;) {
    const uint64_t before = h->hostPassStatsSeq.load(std::memory_order_acquire);
    if (before & 1) {
      YieldProcessor();
      continue;
    }
    result = h->hostPassStatsResult.load(std::memory_order_relaxed);
    std::memcpy(copy, h->hostPassStats, sizeof(copy));
    std::atomic_thread_fence(std::memory_order_acquire);
    if (h->hostPassStatsSeq.load(std::memory_order_relaxed) == before)
      break;
  }
  if (result == REMIXAPI_ERROR_CODE_SUCCESS) {
    std::memcpy(out, copy, size);
    out->structSize = requested ? requested : sizeof(*out);
  }
  return static_cast<remixapi_ErrorCode>(result);
}

// remix_usd_ext.h: mod replacements (the directory is a path on the host,
// which shares the game's file system).
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_ConfigureUSDModsEXT(const remixapi_USDModsOptionsEXT *options) {
  if (!options || options->structSize != sizeof(*options) || options->version != 1)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD();
  Encoder &e = begin(Op::ConfigureUSDMods, sizeof(proto::ArgsUsdMods));
  const uint64_t directory = e.cstr(options->directory);
  e.setArgs(proto::ArgsUsdMods{directory, options->hostMetersPerUnit, options->hostUpAxis,
                               static_cast<uint32_t>(options->watch)});
  return call(e);
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetUSDModsDiagnosticEXT(char *out, uint32_t capacity) {
  if (!out || capacity < 1)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD();
  uint32_t copied = 0;
  const auto r = call(begin(Op::GetUSDModsDiagnostic), out, capacity - 1, &copied);
  out[copied] = 0;
  return r;
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_SetVisibilityPartitionEXT(const remixapi_VisibilityPartitionEXT *partition) {
  if (partition && partition->structSize != sizeof(*partition))
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  GUARD();
  Encoder &e = begin(Op::SetVisibilityPartition, sizeof(proto::ArgsVisibility));
  proto::ArgsVisibility a = {};
  if (partition && partition->nodeCount) {
    a.nodeCount = partition->nodeCount;
    a.leafCount = partition->leafCount;
    a.clusterCount = partition->clusterCount;
    a.rowBytes = partition->rowBytes;
    a.planes = e.bytes(partition->planes, size_t(a.nodeCount) * 4 * sizeof(float));
    a.children = e.bytes(partition->children, size_t(a.nodeCount) * 2 * sizeof(int32_t));
    a.leafClusters = e.bytes(partition->leafClusters, size_t(a.leafCount) * sizeof(int32_t));
    a.visibility = e.bytes(partition->visibility, size_t(a.clusterCount) * a.rowBytes);
  }
  e.setArgs(a);
  return call(e);
}

// Input for the runtime's own UI (F12 menu): the game forwards its window's
// events, which the host replays as SDL events on the runtime window.
// type: 0 mouse move, 1 mouse button (code 1 left, 2 middle, 3 right),
// 2 wheel, 3 key (code: Windows virtual key), 4 text (code: UTF-32),
// 5 focus (down: gained). Coordinates are client pixels.
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_bridge_InjectInputEXT(uint32_t type, uint32_t code, uint32_t down,
                                                                         int32_t x, int32_t y, float wheel) {
  GUARD();
  Encoder &e = begin(Op::InjectInput, sizeof(proto::ArgsInput));
  e.setArgs(proto::ArgsInput{type, code, down, 0, x, y, wheel, 0});
  return post(e);
}

// D3D interop and object picking cannot cross the process boundary.
static remixapi_ErrorCode REMIXAPI_CALL dxvkCreateD3D9(remixapi_Bool, IDirect3D9Ex **) {
  return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
}
static remixapi_ErrorCode REMIXAPI_CALL dxvkRegisterD3D9Device(IDirect3DDevice9Ex *) {
  return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
}
static remixapi_ErrorCode REMIXAPI_CALL dxvkGetExternalSwapchain(uint64_t *, uint64_t *, uint64_t *) {
  return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
}
static remixapi_ErrorCode REMIXAPI_CALL dxvkGetVkImage(IDirect3DSurface9 *, uint64_t *) {
  return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
}
static remixapi_ErrorCode REMIXAPI_CALL dxvkCopyRenderingOutput(IDirect3DSurface9 *,
                                                                remixapi_dxvk_CopyRenderingOutputType) {
  return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
}
static remixapi_ErrorCode REMIXAPI_CALL dxvkSetDefaultOutput(remixapi_dxvk_CopyRenderingOutputType,
                                                             const remixapi_Float4D *) {
  return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
}
static remixapi_ErrorCode REMIXAPI_CALL dxvkGetTextureHash(IDirect3DTexture9 *, uint64_t *) {
  return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
}
static remixapi_ErrorCode REMIXAPI_CALL dxvkGetSharedD3D11TextureHandle(void **, uint32_t *, uint32_t *) {
  return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
}
static remixapi_ErrorCode REMIXAPI_CALL pickRequestObjectPicking(const remixapi_Rect2D *,
                                                                 PFN_remixapi_pick_RequestObjectPickingUserCallback,
                                                                 void *) {
  return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
}
static remixapi_ErrorCode REMIXAPI_CALL pickHighlightObjects(const uint32_t *, uint32_t, uint8_t, uint8_t, uint8_t) {
  return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_InitializeLibrary(const remixapi_InitializeLibraryInfo *info,
                                                                    remixapi_Interface *out) {
  if (!info || info->sType != REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO || !out)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  if (REMIXAPI_VERSION_GET_MAJOR(info->version) != REMIXAPI_VERSION_MAJOR ||
      REMIXAPI_VERSION_GET_MINOR(info->version) > REMIXAPI_VERSION_MINOR)
    return REMIXAPI_ERROR_CODE_INCOMPATIBLE_VERSION;

  Lock guard;
  auto &s = state();
  if (!s.started) {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&remixapi_InitializeLibrary), &self);
    s.log.open(moduleDirectory(self) + L"\\openremix_bridge.log");
    char queued[16];
    DWORD n = GetEnvironmentVariableA("OPENREMIX_BRIDGE_MAX_QUEUED_FRAMES", queued, sizeof(queued));
    if (n > 0 && n < sizeof(queued))
      s.maxQueuedFrames = strtoul(queued, nullptr, 10) ? strtoul(queued, nullptr, 10) : 1;
    std::string error;
    const int32_t result = s.transport.start(s.log, error);
    if (result != 0) {
      s.log.write("bridge: %s", error.c_str());
      s.transport.stop();
      return result == 2 ? REMIXAPI_ERROR_CODE_LOAD_LIBRARY_FAILURE : REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }
    s.started = true;
    s.log.write("bridge: host ready");
  }

  remixapi_Interface api = {};
  api.Shutdown = remixapi_Shutdown;
  api.CreateMaterial = createMaterial;
  api.DestroyMaterial = destroyMaterial;
  api.CreateMesh = createMesh;
  api.CreateMeshBatched = createMesh;
  api.DestroyMesh = destroyMesh;
  api.SetupCamera = setupCamera;
  api.DrawInstance = drawInstance;
  api.CreateLight = createLight;
  api.CreateLightBatched = createLight;
  api.DestroyLight = destroyLight;
  api.DrawLightInstance = drawLightInstance;
  api.SetConfigVariable = setConfigVariable;
  api.AddTextureHash = addTextureHash;
  api.RemoveTextureHash = removeTextureHash;
  api.CreateTexture = createTexture;
  api.DestroyTexture = destroyTexture;
  api.dxvk_CreateD3D9 = dxvkCreateD3D9;
  api.dxvk_RegisterD3D9Device = dxvkRegisterD3D9Device;
  api.dxvk_GetExternalSwapchain = dxvkGetExternalSwapchain;
  api.dxvk_GetVkImage = dxvkGetVkImage;
  api.dxvk_CopyRenderingOutput = dxvkCopyRenderingOutput;
  api.dxvk_SetDefaultOutput = dxvkSetDefaultOutput;
  api.dxvk_GetTextureHash = dxvkGetTextureHash;
  api.dxvk_GetSharedD3D11TextureHandle = dxvkGetSharedD3D11TextureHandle;
  api.pick_RequestObjectPicking = pickRequestObjectPicking;
  api.pick_HighlightObjects = pickHighlightObjects;
  api.Startup = remixapi_Startup;
  api.Present = remixapi_Present;
  api.GetUIState = remixapi_GetUIState;
  api.SetUIState = remixapi_SetUIState;
  api.RegisterCallbacks = remixapi_RegisterCallbacks;
  api.AutoInstancePersistentLights = remixapi_AutoInstancePersistentLights;
  api.UpdateLightDefinition = remixapi_UpdateLightDefinition;
  api.DrawScreenOverlay = drawScreenOverlay;
  api.SetGameValue = remixapi_SetGameValue;
  api.RequestVramCompaction = requestVramCompaction;
  api.GetVramStats = getVramStats;
  api.RequestTextureVramFree = requestTextureVramFree;
  api.GetGameValue = remixapi_GetGameValue;
  api.SetFogState = remixapi_SetFogState;
  api.SetScreenTint = remixapi_SetScreenTint;
  api.RegisterUITexture = remixapi_RegisterUITexture;
  api.FreeUITexture = remixapi_FreeUITexture;
  api.SubmitUIDrawList = remixapi_SubmitUIDrawList;
  api.RequestPresentedScreenshot = remixapi_RequestPresentedScreenshot;
  api.SetCameraMediumMaterial = setCameraMediumMaterial;
  api.PumpEvents = remixapi_PumpEvents;
  api.GetWindowState = remixapi_GetWindowState;
  api.PollMouseState = remixapi_PollMouseState;
  api.IsKeyDown = remixapi_IsKeyDown;
  api.SetMouseGrabbed = remixapi_SetMouseGrabbed;
  api.SetCursorPosition = remixapi_SetCursorPosition;
  api.SetFullscreen = remixapi_SetFullscreen;
  api.GetFeatureAvailability = remixapi_GetFeatureAvailability;
  *out = api;
  return REMIXAPI_ERROR_CODE_SUCCESS;
}

// Command batches: the asynchronous commands issued between BeginRecording
// and EndRecording (sent as usual) are kept by the host under `id`;
// ReplayBatch issues them again with one small command, e.g. a static scene's
// DrawInstance calls every frame. EndRecording fails (and keeps nothing) when
// a command was not self-contained (large payloads).
__declspec(dllexport) remixapi_ErrorCode REMIXAPI_CALL openremix_bridge_BeginRecording(void) {
  GUARD();
  auto &s = state();
  s.recording = true;
  s.recordingFailed = false;
  s.recorded.clear();
  return REMIXAPI_ERROR_CODE_SUCCESS;
}

__declspec(dllexport) remixapi_ErrorCode REMIXAPI_CALL openremix_bridge_EndRecording(uint64_t id) {
  GUARD();
  auto &s = state();
  if (!s.recording)
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  s.recording = false;
  std::vector<uint8_t> records;
  records.swap(s.recorded);
  if (s.recordingFailed || records.empty())
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  Encoder &e = begin(Op::StoreBatch, sizeof(proto::ArgsBatch));
  e.setArgs(proto::ArgsBatch{id, e.bytes(records.data(), records.size()), records.size()});
  return post(e);
}

__declspec(dllexport) remixapi_ErrorCode REMIXAPI_CALL openremix_bridge_ReplayBatch(uint64_t id) {
  GUARD();
  Encoder &e = begin(Op::ReplayBatch, sizeof(proto::ArgsHandle));
  e.setArgs(proto::ArgsHandle{id});
  return post(e);
}

__declspec(dllexport) remixapi_ErrorCode REMIXAPI_CALL openremix_bridge_DeleteBatch(uint64_t id) {
  GUARD();
  Encoder &e = begin(Op::DeleteBatch, sizeof(proto::ArgsHandle));
  e.setArgs(proto::ArgsHandle{id});
  return post(e);
}

// Diagnostics for hosts that want to surface bridge failures.
__declspec(dllexport) uint32_t REMIXAPI_CALL openremix_bridge_GetAsyncErrorCount(void) {
  Lock guard;
  return state().started ? state().transport.asyncErrors() : 0;
}

} // extern "C"

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID reserved) {
  // On FreeLibrary shut the host down cleanly; at process exit the job object
  // and the host's parent watch take care of it.
  if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {
    Lock guard;
    if (state().started) {
      state().transport.stop();
      state().started = false;
    }
  }
  return TRUE;
}
