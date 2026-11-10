#include "frame_profile.h"
#include "diag.h"
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <memory>
#include <fcntl.h>
#include <io.h>
#include <remix/remix_native_ext.h>
#include <remix/remix_bake_ext.h>

namespace proxy::frame_profile {
namespace {
std::atomic<uint32_t> worldListsUs{0}, worldPatchesUs{0};
struct Recorder {
  using File = std::unique_ptr<FILE, decltype(&std::fclose)>;
  File file{nullptr, &std::fclose};
  PFN_remixapi_GetGpuPassTimingsEXT gpu = nullptr;
  PFN_remixapi_GetFramePerformanceEXT cpu = nullptr;
  PFN_remixapi_GetBakeStatisticsEXT bake = nullptr;
  uint64_t lastGpu = ~0ull, lastCpu = ~0ull;
  uint32_t rows = 0;
  double previousSampleMilliseconds = 0;

  Recorder() {
    wchar_t path[32768] = {};
    const auto length = GetEnvironmentVariableW(L"OPENREMIX_PROXY_FRAME_PROFILE", path, 32768);
    if (!length || length >= 32768) return;
    // Never overwrite an earlier measurement accidentally.
    const auto handle = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
      const int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_WRONLY | _O_TEXT);
      if (descriptor >= 0) {
        file.reset(_fdopen(descriptor, "w"));
        if (!file) _close(descriptor);
      } else {
        CloseHandle(handle);
      }
    }
    if (!file) {
      diag::stage("Frame profile: cannot create new output file");
      return;
    }
    const auto module = GetModuleHandleW(L"remix.dll");
    gpu = reinterpret_cast<PFN_remixapi_GetGpuPassTimingsEXT>(GetProcAddress(module, "remixapi_GetGpuPassTimingsEXT"));
    cpu = reinterpret_cast<PFN_remixapi_GetFramePerformanceEXT>(GetProcAddress(module, "remixapi_GetFramePerformanceEXT"));
    bake = reinterpret_cast<PFN_remixapi_GetBakeStatisticsEXT>(GetProcAddress(module, "remixapi_GetBakeStatisticsEXT"));
    std::fputs("unixMs,hostMs,presentStatus,cpuRecordMs,cpuFenceMs,gpuAvailable,gpuNew,gpuFrame,gpuMs,renderWidth,renderHeight,"
               "outputWidth,outputHeight,opaqueMs,clusterMs,localShadowMs,sunShadowMs,reflectionMs,"
               "depthMs,viewmodelMs,viewmodelSunShadowMs,viewmodelLocalShadowMs,bakeState,bakeHash,"
               "hostFocused,processFocused,windowStatus,nativeFocused,nativeMinimized,nativeWidth,nativeHeight,cpuStatus,bakeStatus,"
               "gpuTruncated,probeGatherAvailable,probeGatherMs,viewmodelColorMs,viewmodelDepthMs,"
               "cpuAttempt,cpuNew,cpuPresented,cpuPresentMs,cpuSceneMs,cpuAcquireMs,cpuSubmitMs,cpuQueuePresentMs,profilePreviousMs,"
               "hostStagesValid,hostProxyDrawMs,hostMeshBuildMs,hostSubmitMs,hostBspMs,hostPresentCallMs,"
               "hostWorldListsMs,hostWorldPatchesMs\n", file.get());
  }

  void sample(double hostMs, remixapi_ErrorCode presentStatus,
              HWND hostWindow, PFN_remixapi_GetWindowState getWindowState, const HostStages &stages) {
    if (!file) return;
    const auto began = std::chrono::steady_clock::now();
    // Record observations without changing focus. Missing window state is an
    // explicit failure status, never an assumed focused measurement.
    remixapi_WindowState window{};
    const auto windowStatus = getWindowState ? getWindowState(&window) : REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    const auto foreground = GetForegroundWindow();
    DWORD foregroundProcess = 0;
    if (foreground) GetWindowThreadProcessId(foreground, &foregroundProcess);
    const bool hostFocused = hostWindow && foreground == hostWindow;
    const bool processFocused = foregroundProcess == GetCurrentProcessId();
    remixapi_GpuPassTimingsEXT timings{};
    timings.structSize = sizeof(timings);
    const bool available = presentStatus == REMIXAPI_ERROR_CODE_SUCCESS && gpu &&
                           gpu(&timings) == REMIXAPI_ERROR_CODE_SUCCESS && timings.available;
    remixapi_FramePerformanceEXT frame{};
    frame.structSize = sizeof(frame);
    const auto cpuStatus = cpu ? cpu(&frame) : REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    remixapi_BakeStatisticsEXT lighting{};
    lighting.structSize = sizeof(lighting);
    const auto bakeStatus = bake ? bake(&lighting) : REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    auto findPass = [&](const char *name) -> const remixapi_GpuPassTimingEXT * {
      if (available)
        for (uint32_t i = 0; i < timings.passCount && i < REMIXAPI_GPU_PASS_TIMING_CAPACITY_EXT; ++i)
          if (std::strcmp(timings.passes[i].name, name) == 0) return &timings.passes[i];
      return nullptr;
    };
    auto pass = [&](const char *name) {
      const auto *range = findPass(name);
      return range ? range->milliseconds : 0.0;
    };
    const auto *probeGather = findPass("Forward+ selected probe visibility gather");
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    const uint64_t ticks = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    const auto unixMs = (ticks - 116444736000000000ull) / 10000;
    const bool fresh = available && timings.frame != lastGpu;
    // These fields come from the same CPU snapshot already queried above. Host
    // intervals and completed CPU/GPU frames are asynchronous; record identity
    // and freshness rather than treating repeated snapshots as new frames.
    const bool cpuFresh = cpuStatus == REMIXAPI_ERROR_CODE_SUCCESS && frame.attempts != lastCpu;
    std::fprintf(file.get(), "%llu,%.6f,%u,%.6f,%.6f,%u,%u,%llu,%.6f,%u,%u,%u,%u,%.6f,%.6f,%.6f,%.6f,%.6f,"
        "%.6f,%.6f,%.6f,%.6f,%u,%llu,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%.6f,%.6f,%.6f,"
        "%llu,%u,%u,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%u,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n",
        static_cast<unsigned long long>(unixMs), hostMs, unsigned(presentStatus), frame.recordMilliseconds, frame.fenceMilliseconds,
        unsigned(available), unsigned(fresh), static_cast<unsigned long long>(timings.frame), timings.frameMilliseconds,
        timings.renderWidth, timings.renderHeight, timings.outputWidth, timings.outputHeight,
        pass("Forward+ opaque world shading"), pass("Forward+ light clustering"),
        pass("Forward+ local shadow maps"), pass("Forward+ sun shadow cascades"),
        pass("Forward+ reflection resolve and composition"), pass("Forward+ opaque depth prepass"),
        pass("Forward+ lit viewmodel overlay"), pass("Forward+ viewmodel sun shadow"),
        pass("Forward+ viewmodel local self shadows"), lighting.state,
        static_cast<unsigned long long>(lighting.activeHash), unsigned(hostFocused), unsigned(processFocused),
        unsigned(windowStatus), unsigned(window.focused), unsigned(window.minimized),
        window.drawableWidth, window.drawableHeight, unsigned(cpuStatus), unsigned(bakeStatus),
        timings.truncated, unsigned(probeGather != nullptr), probeGather ? probeGather->milliseconds : 0.0,
        pass("Forward+ viewmodel color"), pass("Forward+ viewmodel depth prepass"),
        static_cast<unsigned long long>(frame.attempts), unsigned(cpuFresh), frame.presented,
        frame.presentMilliseconds, frame.sceneMilliseconds, frame.acquireMilliseconds,
        frame.submitMilliseconds, frame.queuePresentMilliseconds, previousSampleMilliseconds,
        unsigned(stages.valid), stages.draws, stages.meshBuild, stages.submission, stages.bsp, stages.present,
        stages.worldLists, stages.worldPatches);
    if (available) lastGpu = timings.frame;
    if (cpuStatus == REMIXAPI_ERROR_CODE_SUCCESS) lastCpu = frame.attempts;
    if (++rows % 120 == 0) std::fflush(file.get());
    // A row cannot include the cost of writing itself. Attribute this complete
    // sample (queries, formatting and any flush) to the next row explicitly.
    previousSampleMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - began).count();
  }
};
Recorder &recorder() {
  static Recorder instance;
  return instance;
}
}

bool enabled() { return bool(recorder().file); }

void recordWorldLists(uint32_t lists, uint32_t patches) {
  worldListsUs.fetch_add(lists, std::memory_order_relaxed);
  worldPatchesUs.fetch_add(patches, std::memory_order_relaxed);
}

WorldListTotals worldListTotals() {
  return {worldListsUs.load(std::memory_order_relaxed), worldPatchesUs.load(std::memory_order_relaxed)};
}

void sample(double hostFrameMilliseconds, remixapi_ErrorCode presentStatus,
            HWND hostWindow, PFN_remixapi_GetWindowState getWindowState, const HostStages &stages) {
  recorder().sample(hostFrameMilliseconds, presentStatus, hostWindow, getWindowState, stages);
}
}
