#include "replay_timing.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <stdexcept>

namespace proxy::replay {
namespace {
std::string quote(const std::string &text) {
  std::string result = "\"";
  for (unsigned char c : text) {
    if (c == '\\' || c == '"')
      result += '\\';
    if (c < 32)
      throw std::runtime_error("Control character in timing metadata");
    result += char(c);
  }
  return result + '"';
}
} // namespace
void TimingWindow::reset() {
  m_warmed = m_truncated = 0;
  m_observedStart = m_observedEnd = 0;
  m_warmupStart = {};
  m_samples.clear();
  m_passes.clear();
}
void TimingWindow::observe(bool ready, uint32_t backend, uint64_t hash,
                           const remixapi_GpuPassTimingsEXT &timing) {
  if (!enabled() || complete())
    return;
  if (!ready) {
    reset();
    m_key = false;
    return;
  }
  if (!timing.available || !std::isfinite(timing.frameMilliseconds) || timing.frameMilliseconds < 0 ||
      !timing.renderWidth || !timing.renderHeight || !timing.timestampValidBits)
    return;
  if (!m_key || m_backend != backend || m_hash != hash || m_width != timing.renderWidth ||
      m_height != timing.renderHeight || m_outputWidth != timing.outputWidth ||
      m_outputHeight != timing.outputHeight) {
    reset();
    m_key = true;
    m_backend = backend;
    m_hash = hash;
    m_width = timing.renderWidth;
    m_height = timing.renderHeight;
    m_outputWidth = timing.outputWidth;
    m_outputHeight = timing.outputHeight;
  }
  if (m_seen && timing.frame <= m_lastFrame)
    return;
  m_seen = true;
  m_lastFrame = timing.frame;
  const auto now = std::chrono::steady_clock::now();
  if (m_warmupStart == std::chrono::steady_clock::time_point{})
    m_warmupStart = now;
  if (m_warmed < m_warmup || std::chrono::duration<double>(now - m_warmupStart).count() < m_warmupSeconds) {
    ++m_warmed;
    return;
  }
  if (timing.passCount > REMIXAPI_GPU_PASS_TIMING_CAPACITY_EXT)
    throw std::runtime_error("Invalid GPU timing pass count");
  m_observedEnd = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
  if (m_samples.empty())
    m_observedStart = m_observedEnd;
  m_samples.push_back({timing.frame, timing.frameMilliseconds});
  m_truncated += timing.truncated != 0;
  std::map<std::string, Pass> frame;
  for (uint32_t i = 0; i < timing.passCount; ++i) {
    const auto &source = timing.passes[i];
    if (!std::isfinite(source.milliseconds) || !std::isfinite(source.selfMilliseconds) ||
        source.milliseconds < 0 || source.selfMilliseconds < 0)
      continue;
    const auto end = std::find(std::begin(source.name), std::end(source.name), '\0');
    auto &pass = frame[std::string(source.name, end)];
    pass.inclusive += source.milliseconds;
    pass.self += source.selfMilliseconds;
  }
  for (const auto &[name, source] : frame) {
    auto &pass = m_passes[name];
    pass.inclusive += source.inclusive;
    pass.self += source.self;
    ++pass.frames;
  }
}
void TimingWindow::write(const std::filesystem::path &output, const std::string &map) const {
  if (!complete())
    throw std::runtime_error("GPU timing window incomplete: collected " + std::to_string(samples()) + " of " +
                             std::to_string(m_count) + " frames; increase the replay frame limit");
  auto path = output;
  path.replace_extension(".timings.csv");
  std::ofstream csv(path);
  csv << std::setprecision(12) << "gpu_frame,gpu_ms\n";
  std::vector<double> sorted;
  for (const auto &sample : m_samples) {
    csv << sample.frame << ',' << sample.milliseconds << '\n';
    sorted.push_back(sample.milliseconds);
  }
  std::sort(sorted.begin(), sorted.end());
  const double mean = std::accumulate(sorted.begin(), sorted.end(), 0.) / sorted.size();
  auto percentile = [&](double q) { return sorted[size_t(std::ceil(q * sorted.size())) - 1]; };
  path.replace_extension(".json");
  std::ofstream json(path);
  json << std::setprecision(12) << "{\n  \"map\": " << quote(map) << ",\n  \"backend\": " << m_backend
       << ",\n  \"bakeHash\": " << quote(std::to_string(m_hash)) << ",\n  \"renderSize\": [" << m_width << ','
       << m_height << "],\n  \"outputSize\": [" << m_outputWidth << ',' << m_outputHeight
       << "],\n  \"warmupFrames\": " << m_warmed << ",\n  \"minimumWarmupSeconds\": " << m_warmupSeconds
       << ",\n  \"samples\": " << samples() << ",\n  \"truncatedFrames\": " << m_truncated
       << ",\n  \"observedStartUnixMs\": " << m_observedStart
       << ",\n  \"observedEndUnixMs\": " << m_observedEnd << ",\n  \"gpuMeanMs\": " << mean
       << ",\n  \"gpuMedianMs\": " << percentile(.5) << ",\n  \"gpuP95Ms\": " << percentile(.95)
       << ",\n  \"passes\": [";
  bool first = true;
  for (const auto &[name, pass] : m_passes) {
    json << (first ? "" : ",") << "\n    {\"name\": " << quote(name) << ", \"frames\": " << pass.frames
         << ", \"meanInclusiveMs\": " << pass.inclusive / pass.frames
         << ", \"meanSelfMs\": " << pass.self / pass.frames << '}';
    first = false;
  }
  json << "\n  ]\n}\n";
  csv.flush();
  json.flush();
  if (!csv || !json)
    throw std::runtime_error("Cannot write GPU timing report");
}
} // namespace proxy::replay
