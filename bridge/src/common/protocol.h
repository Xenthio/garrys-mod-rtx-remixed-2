#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

// Shared-memory transport between the x86 client (in the game process) and
// the x64 host (which owns the OpenRemix runtime).
//
// Memory: [Header][Reply area][Command ring]. The client is the only writer
// of the ring and the host the only reader. Records are 16-byte aligned and
// never straddle the end of the ring (a PAD record fills the tail instead).
//
// Struct arguments are written in their x64 layout. Pointer fields hold an
// encoded reference (see PtrTag) that the host resolves in place before it
// calls the runtime, so bulk data is read directly from shared memory.
namespace bridge::proto {

inline constexpr uint32_t kMagic = 0x5242524f; // "ORBR"
inline constexpr uint32_t kVersion = 2; // 2: scene instances, host passes, texture updates
inline constexpr uint32_t kHeaderSize = 4096;
inline constexpr uint32_t kReplySize = 64 * 1024;
inline constexpr uint32_t kDefaultRingSize = 64u * 1024u * 1024u;
inline constexpr uint32_t kRecordAlign = 16;

// Encoded pointer: top two bits select how the low 62 bits are interpreted.
enum PtrTag : uint64_t {
  kPtrRecord = 1ull << 62, // byte offset from the start of the current record
  kPtrRing = 2ull << 62,   // absolute (monotonic) ring position of a DATA payload
  kPtrBlob = 3ull << 62,   // id of a blob reassembled from BLOB_CHUNK records
  kPtrTagMask = 3ull << 62,
};

enum class HostState : uint32_t { Starting = 0, Ready = 1, Failed = 2, Exited = 3 };

struct alignas(64) Header {
  uint32_t magic;
  uint32_t version;
  uint32_t headerSize;
  uint32_t replySize;
  uint32_t ringSize;
  uint32_t clientPid;
  uint32_t hostPid;
  uint32_t reserved0;

  alignas(64) std::atomic<uint64_t> writePos; // bytes published by the client
  alignas(64) std::atomic<uint64_t> readPos;  // bytes released by the host
  alignas(64) std::atomic<uint32_t> hostSleeping;
  std::atomic<uint32_t> clientSleeping;
  std::atomic<uint32_t> hostState;
  std::atomic<uint32_t> asyncErrors;
  std::atomic<uint32_t> clientClosing;   // host exits once the ring drains
  uint32_t reserved1;
  std::atomic<uint64_t> syncDone;         // seq of the last completed synchronous command
  std::atomic<uint64_t> framesPresented;  // Present commands executed by the host
  char lastError[512];

  // Answers for per-frame getters, refreshed by the host after Startup, every
  // Present and every SetConfigVariable/SetConfigDefault so the client never
  // waits for them (a synchronous call waits for the queued frames).
  // remixapi_GetHostPassesEnabledEXT: kHostPassesUnavailable while the
  // runtime lacks it, else the remixapi_Bool it returned.
  std::atomic<uint32_t> hostPassesEnabled;
  // RuntimeExport bits: optional named exports remix.dll has. Written before
  // hostState becomes Ready; the client's exports for absent ones return
  // FEATURE_UNAVAILABLE instead of failing asynchronously in the host.
  uint32_t runtimeExports;
  // remixapi_GetHostPassStatisticsEXT: its result code and the struct, under
  // a sequence lock (odd while the host writes).
  std::atomic<int32_t> hostPassStatsResult;
  std::atomic<uint64_t> hostPassStatsSeq;
  alignas(8) uint8_t hostPassStats[256];
};
static_assert(sizeof(Header) <= kHeaderSize);
inline constexpr uint32_t kHostPassesUnavailable = 0xffffffffu;
enum RuntimeExport : uint32_t {
  kExportConfigDefault = 1u << 0,
  kExportUpdateTexture = 1u << 1,
  kExportUpdateUITextureRegion = 1u << 2,
  kExportSceneInstances = 1u << 3, // Add, Set, Change, Remove, Draw
  kExportHostPasses = 1u << 4,     // RegisterHostShader, DestroyHostShader, SubmitHostPasses, UpdateHostTarget
  kExportHostVolume = 1u << 5,
  kExportHostPassStatistics = 1u << 6,
};
static_assert(std::atomic<uint64_t>::is_always_lock_free);

struct Reply {
  uint64_t seq;
  int32_t result;
  uint32_t size; // payload bytes following this struct
};

enum class Op : uint32_t {
  Pad = 0,
  Data,        // raw payload referenced by later records through kPtrRing
  BlobBegin,   // { id, totalSize }
  BlobChunk,   // { id, offset, bytes... }
  Indirect,    // { blob id }: execute a record too large for the ring from a blob

  Startup,
  Shutdown,
  Present,
  CreateMaterial,
  DestroyMaterial,
  CreateMesh,
  DestroyMesh,
  SetupCamera,
  DrawInstance,
  CreateLight,
  DestroyLight,
  DrawLightInstance,
  UpdateLightDefinition,
  AutoInstancePersistentLights,
  SetConfigVariable,
  AddTextureHash,
  RemoveTextureHash,
  CreateTexture,
  DestroyTexture,
  GetUIState,
  SetUIState,
  DrawScreenOverlay,
  SetGameValue,
  GetGameValue,
  RequestVramCompaction,
  GetVramStats,
  RequestTextureVramFree,
  SetFogState,
  SetScreenTint,
  RegisterUITexture,
  FreeUITexture,
  SubmitUIDrawList,
  RequestPresentedScreenshot,
  SetCameraMediumMaterial,
  PumpEvents,
  GetWindowState,
  PollMouseState,
  IsKeyDown,
  SetMouseGrabbed,
  SetCursorPosition,
  SetFullscreen,
  GetFeatureAvailability,
  PollInputEvents,
  SetTextInputEnabled,
  GetWindowMetrics,
  GetPresentationCount,
  GetStatistics, // generic out-struct getter, see StatKind
  SetConfigDefault,      // ArgsStrings
  UpdateTexture,         // ArgsRegion (handle: client texture id)
  UpdateUITextureRegion, // ArgsRegion (handle: UI texture handle)
  // Scene instances: client-assigned ids, mapped to runtime handles by the host.
  AddSceneInstances,     // ArgsSceneAdd
  SetSceneInstance,      // ArgsCreate
  ChangeSceneInstances,  // ArgsArray of remixapi_SceneInstanceChangeEXT (ids)
  RemoveSceneInstances,  // ArgsArray of uint64_t ids
  DrawSceneInstances,    // ArgsOrigin
  // Host passes: shader handles are client-assigned ids, host targets are
  // the game's own values (passed through).
  RegisterHostShader,    // ArgsCreate
  DestroyHostShader,     // ArgsHandle
  SubmitHostPasses,      // ArgsInfo
  UpdateHostTarget,      // ArgsHostImage (depth 0)
  UpdateHostVolume,      // ArgsHostImage
  // remix_usd_ext.h
  ConfigureUSDMods,
  GetUSDModsDiagnostic,
  InjectInput,
  SetVisibilityPartition,
  // openremix_bridge_*Recording: client-recorded command batches kept by the
  // host and replayed by id (no runtime counterpart).
  StoreBatch,  // ArgsBatch: the batch's records
  ReplayBatch, // ArgsHandle
  DeleteBatch, // ArgsHandle
  Count,
};

// Extension getters whose single argument is a fixed-size struct that is
// identical on both ABIs and starts with `uint32_t structSize`.
enum class StatKind : uint32_t {
  BackendCapabilities,
  LocalLight,
  LocalLightGuiding,
  Shadow,
  Sharc,
  Tracing,
  FramePerformance,
  FrameBreakdown,
  GpuPassTimings,
  DenoisingCapabilities,
  Denoising,
  Texture,
  CameraCulling,
  LocalLightSet,
  Count,
};

struct Record {
  Op op;
  uint32_t size; // total bytes including this header, multiple of kRecordAlign
  uint64_t seq;  // nonzero when the client waits for a Reply
};
static_assert(sizeof(Record) == 16);

// Fixed argument blocks that follow a Record. Pointers are encoded (PtrTag).
struct ArgsHandle { uint64_t id; };
struct ArgsCreate { uint64_t id; uint64_t info; };
struct ArgsInfo { uint64_t info; };
struct ArgsStrings { uint64_t a; uint64_t b; };
struct ArgsU32 { uint32_t value; uint32_t pad; };
struct ArgsF4 { float v[4]; };
struct ArgsBlobBegin { uint64_t id; uint64_t size; };
struct ArgsBlobChunk { uint64_t id; uint64_t offset; uint64_t size; };
struct ArgsOverlay { uint64_t pixels; uint32_t width, height, format; float opacity; };
struct ArgsUITexture { uint64_t handle; uint64_t data; uint64_t dataSize; uint32_t width, height, format, pad; };
struct ArgsGameValue { uint64_t key; uint32_t capacity, pad; };
struct ArgsStat { StatKind kind; uint32_t size; };
struct ArgsLightUpdate { uint64_t id; uint64_t info; };
struct ArgsArray { uint64_t items; uint32_t count, pad; };      // encoded pointer + element count
struct ArgsOrigin { uint32_t hasOrigin; float origin[3]; };
struct ArgsUsdMods { uint64_t directory; double hostMetersPerUnit; uint32_t hostUpAxis, watch; };
// remixapi_bridge_InjectInputEXT: one input event of the game window, for the
// runtime's own UI (its SDL window cannot receive another process's input).
// remixapi_SetVisibilityPartitionEXT: inline arrays (0 offsets: clear).
struct ArgsVisibility {
  uint64_t planes, children, leafClusters, visibility;
  uint32_t nodeCount, leafCount, clusterCount, rowBytes;
};
struct ArgsBatch { uint64_t id; uint64_t data; uint64_t size; };
// A region of a texture: tightly packed rows (rowPitch = width * texel size).
struct ArgsRegion { uint64_t handle; uint64_t pixels; uint32_t x, y, width, height, format, rowPitch; };
// ids: count uint64_t client ids; infos: count InstanceInfo images.
struct ArgsSceneAdd { uint64_t ids; uint64_t infos; uint32_t count, pad; };
struct ArgsHostImage { uint64_t target; uint64_t pixels; uint64_t bytes; uint32_t width, height, depth, format; };
struct ArgsInput { uint32_t type, code, down, pad; int32_t x, y; float wheel, pad2; };

inline constexpr uint32_t alignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

inline const char *opName(Op op) {
  static const char *const names[] = {
      "Pad", "Data", "BlobBegin", "BlobChunk", "Indirect", "Startup", "Shutdown", "Present",
      "CreateMaterial", "DestroyMaterial", "CreateMesh", "DestroyMesh", "SetupCamera",
      "DrawInstance", "CreateLight", "DestroyLight", "DrawLightInstance",
      "UpdateLightDefinition", "AutoInstancePersistentLights", "SetConfigVariable",
      "AddTextureHash", "RemoveTextureHash", "CreateTexture", "DestroyTexture", "GetUIState",
      "SetUIState", "DrawScreenOverlay", "SetGameValue", "GetGameValue",
      "RequestVramCompaction", "GetVramStats", "RequestTextureVramFree", "SetFogState",
      "SetScreenTint", "RegisterUITexture", "FreeUITexture", "SubmitUIDrawList",
      "RequestPresentedScreenshot", "SetCameraMediumMaterial", "PumpEvents", "GetWindowState",
      "PollMouseState", "IsKeyDown", "SetMouseGrabbed", "SetCursorPosition", "SetFullscreen",
      "GetFeatureAvailability", "PollInputEvents", "SetTextInputEnabled", "GetWindowMetrics",
      "GetPresentationCount", "GetStatistics", "SetConfigDefault", "UpdateTexture", "UpdateUITextureRegion",
      "AddSceneInstances", "SetSceneInstance", "ChangeSceneInstances", "RemoveSceneInstances",
      "DrawSceneInstances", "RegisterHostShader", "DestroyHostShader", "SubmitHostPasses", "UpdateHostTarget",
      "UpdateHostVolume", "ConfigureUSDMods", "GetUSDModsDiagnostic", "InjectInput",
      "SetVisibilityPartition", "StoreBatch", "ReplayBatch", "DeleteBatch"};
  static_assert(sizeof(names) / sizeof(names[0]) == static_cast<size_t>(Op::Count));
  return static_cast<uint32_t>(op) < static_cast<uint32_t>(Op::Count) ? names[static_cast<uint32_t>(op)]
                                                                      : "?";
}

} // namespace bridge::proto
