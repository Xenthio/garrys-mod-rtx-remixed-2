#include "replay_timing.h"
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace {
void require(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
  }
}
std::string read(const std::filesystem::path &path) {
  std::ifstream file(path);
  std::ostringstream text;
  text << file.rdbuf();
  return text.str();
}
} // namespace
int main() {
  proxy::replay::TimingWindow window(3, 2);
  remixapi_GpuPassTimingsEXT timing{};
  timing.available = 1;
  timing.renderWidth = timing.outputWidth = 640;
  timing.renderHeight = timing.outputHeight = 360;
  timing.timestampValidBits = 64;
  timing.passCount = 2;
  for (auto &pass : timing.passes) {
    std::snprintf(pass.name, sizeof(pass.name), "opaque");
    pass.milliseconds = 1.5;
    pass.selfMilliseconds = 1;
  }
  auto observe = [&](uint64_t frame, bool ready = true, uint64_t hash = 1, double ms = 1) {
    timing.frame = frame;
    timing.frameMilliseconds = ms;
    window.observe(ready, 2, hash, timing);
  };
  observe(1, false);
  observe(2);
  observe(2);
  observe(3);
  require(window.samples() == 0, "Duplicate GPU frames must not advance warmup");
  observe(4);
  require(window.samples() == 1, "Distinct warmup frames precede measurement");
  observe(5, false);
  require(window.samples() == 0, "Losing readiness discards a partial window");
  observe(6);
  observe(7);
  observe(8);
  require(window.samples() == 1, "Measurement resumes after readiness warmup");
  observe(9, true, 2);
  observe(10, true, 2);
  require(window.samples() == 0, "A changed bake restarts the warmup window");
  observe(11, true, 2, 1);
  observe(11, true, 2, 1000);
  timing.truncated = 1;
  observe(12, true, 2, 2);
  timing.truncated = 0;
  observe(13, true, 2, 9);
  require(window.complete() && window.samples() == 3, "Only distinct frames fill the measurement window");
  const auto base = std::filesystem::path("replay-timing-test.png");
  window.write(base, "map\"test");
  const auto json = read("replay-timing-test.timings.json"), csv = read("replay-timing-test.timings.csv");
  require(json.find("\"gpuMeanMs\": 4") != std::string::npos &&
              json.find("\"gpuMedianMs\": 2") != std::string::npos &&
              json.find("\"gpuP95Ms\": 9") != std::string::npos &&
              json.find("\"meanInclusiveMs\": 3") != std::string::npos &&
              json.find("\"truncatedFrames\": 1") != std::string::npos &&
              json.find("map\\\"test") != std::string::npos,
          "Summary statistics, pass aggregation and JSON escaping must agree with raw samples");
  require(csv.find("11,1\n12,2\n13,9\n") != std::string::npos && csv.find("1000") == std::string::npos,
          "CSV retains only the accepted GPU frame samples");
  bool refused = false;
  try {
    proxy::replay::TimingWindow(2, 2).write(base, "test");
  } catch (const std::runtime_error &) {
    refused = true;
  }
  require(refused, "An incomplete run must not emit a successful measurement");
  proxy::replay::TimingWindow changing(2, 1);
  timing.frame = 20;
  timing.frameMilliseconds = 1;
  changing.observe(true, 2, 1, timing);
  timing.frame = 21;
  changing.observe(true, 2, 1, timing);
  ++timing.renderWidth;
  timing.frame = 22;
  changing.observe(true, 2, 1, timing);
  require(changing.samples() == 0, "A resolution change must restart warmup");
  timing.frame = 23;
  changing.observe(true, 2, 1, timing);
  timing.frame = 24;
  changing.observe(true, 1, 1, timing);
  require(changing.samples() == 0, "A backend change must restart warmup");
  timing.frame = 25;
  changing.observe(true, 1, 1, timing);
  timing.available = 0;
  timing.frame = 26;
  changing.observe(true, 1, 1, timing);
  timing.available = 1;
  timing.frame = 27;
  timing.frameMilliseconds = std::numeric_limits<double>::quiet_NaN();
  changing.observe(true, 1, 1, timing);
  require(changing.samples() == 1, "Unavailable and non-finite timings must not become samples");
  timing.frame = 28;
  timing.frameMilliseconds = 1;
  changing.observe(true, 1, 1, timing);
  require(changing.complete(), "Valid timings recover after rejected samples");
  proxy::replay::TimingWindow timed(1, 0, 60);
  timing.frame = 29;
  timed.observe(true, 2, 1, timing);
  require(timed.samples() == 0 && timed.warmedFrames() == 1,
          "Elapsed-time warmup must still apply after the frame requirement is met");
  std::filesystem::remove("replay-timing-test.timings.json");
  std::filesystem::remove("replay-timing-test.timings.csv");
  std::puts("GPU timing readiness, deduplication, warmup, resets and report math passed");
}
