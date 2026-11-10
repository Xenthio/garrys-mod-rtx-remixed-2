#pragma once
#include <windows.h>
#include <remix/remix_native_ext.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace proxy::replay {
// CPU snapshots describe completed Present attempts, independently of delayed
// GPU queries. Log their identity instead of pretending they are the same frame.
class HostProfile {
public:
  HostProfile(HMODULE library, const std::filesystem::path &image, bool enabled) {
    if (!enabled) return;
    m_performance = reinterpret_cast<PFN_remixapi_GetFramePerformanceEXT>(
        GetProcAddress(library, "remixapi_GetFramePerformanceEXT"));
    m_breakdown = reinterpret_cast<PFN_remixapi_GetFrameBreakdownEXT>(
        GetProcAddress(library, "remixapi_GetFrameBreakdownEXT"));
    if (!m_performance || !m_breakdown) return; // older runtimes retain GPU-only measurements
    auto path = image;
    path.replace_extension(".host.csv");
    if (std::filesystem::exists(path)) throw std::runtime_error("Host profile output already exists");
    m_output.open(path);
    m_output.exceptions(std::ios::failbit | std::ios::badbit);
    m_output.precision(12);
    m_output << "unixMs,ready,backend,bakeHash,hostMs,cpuAttempt,cpuPresentMs,cpuSceneMs,cpuRecordMs,"
                "cpuFenceMs,cpuSetupMs,cpuReleaseMs,rasterPrepareMs,passesMs,rasterDraws\n";
  }
  void sample(double hostMs, bool ready, uint32_t backend, uint64_t hash) {
    if (!m_output.is_open()) return;
    remixapi_FramePerformanceEXT cpu{};
    cpu.structSize = sizeof(cpu);
    remixapi_FrameBreakdownEXT breakdown{};
    breakdown.structSize = sizeof(breakdown);
    if (m_performance(&cpu) != REMIXAPI_ERROR_CODE_SUCCESS || !cpu.presented ||
        cpu.attempts == m_lastAttempt || m_breakdown(&breakdown) != REMIXAPI_ERROR_CODE_SUCCESS) return;
    // Separate diagnostic queries can straddle an asynchronous completion.
    // Refuse that row rather than combine two different CPU snapshots.
    remixapi_FramePerformanceEXT confirmed{};
    confirmed.structSize = sizeof(confirmed);
    if (m_performance(&confirmed) != REMIXAPI_ERROR_CODE_SUCCESS ||
        confirmed.attempts != cpu.attempts) return;
    m_lastAttempt = cpu.attempts;
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    m_output << now << ',' << ready << ',' << backend << ',' << hash << ',' << hostMs << ','
             << cpu.attempts << ',' << cpu.presentMilliseconds << ',' << cpu.sceneMilliseconds << ','
             << cpu.recordMilliseconds << ',' << cpu.fenceMilliseconds << ','
             << breakdown.presentSetupMilliseconds << ',' << breakdown.presentReleaseMilliseconds << ','
             << breakdown.rasterPrepareMilliseconds << ',' << breakdown.passesMilliseconds << ','
             << breakdown.rasterDraws << '\n';
  }
private:
  PFN_remixapi_GetFramePerformanceEXT m_performance = nullptr;
  PFN_remixapi_GetFrameBreakdownEXT m_breakdown = nullptr;
  std::ofstream m_output;
  uint64_t m_lastAttempt = 0;
};
} // namespace proxy::replay
