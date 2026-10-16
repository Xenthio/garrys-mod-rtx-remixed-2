#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <remix/remix_native_ext.h>
#include <vector>

namespace proxy::replay {
// GPU results arrive several frames late. Warm up using distinct completed GPU
// frames, and restart the window when the backend or baked input changes.
class TimingWindow {
public:
  TimingWindow(uint32_t count, uint32_t warmup, double seconds = 0)
      : m_count(count), m_warmup(warmup), m_warmupSeconds(seconds) {}
  void observe(bool ready, uint32_t backend, uint64_t bakeHash, const remixapi_GpuPassTimingsEXT &timing);
  bool enabled() const { return m_count != 0; }
  bool complete() const { return m_count && m_samples.size() == m_count; }
  size_t samples() const { return m_samples.size(); }
  uint32_t warmedFrames() const { return m_warmed; }
  void write(const std::filesystem::path &output, const std::string &map) const;

private:
  struct Sample {
    uint64_t frame;
    double milliseconds;
  };
  struct Pass {
    double inclusive = 0, self = 0;
    uint32_t frames = 0;
  };
  void reset();
  uint32_t m_count, m_warmup, m_warmed = 0, m_backend = 0, m_truncated = 0;
  uint64_t m_hash = 0, m_lastFrame = 0;
  int64_t m_observedStart = 0, m_observedEnd = 0;
  double m_warmupSeconds = 0;
  std::chrono::steady_clock::time_point m_warmupStart{};
  bool m_key = false, m_seen = false;
  uint32_t m_width = 0, m_height = 0, m_outputWidth = 0, m_outputHeight = 0;
  std::vector<Sample> m_samples;
  std::map<std::string, Pass> m_passes;
};
} // namespace proxy::replay
