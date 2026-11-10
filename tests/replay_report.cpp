#include "replay_report.h"
#include <chrono>
#include <cstdio>

int main() try {
  const auto directory = std::filesystem::temp_directory_path() /
      ("openremix-report-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  if (!std::filesystem::create_directory(directory)) throw std::runtime_error("Test directory collision");
  const auto output = directory / "probes.json", request = directory / "probes.request";
  std::ofstream(output) << "{\"activeHash\":\"42\"}";
  const auto originalTime = std::filesystem::last_write_time(output);
  proxy::replay::FreshReport report(output, request);
  report.request();
  if (report.ready() || std::filesystem::exists(output) || !std::filesystem::exists(request))
    throw std::runtime_error("A previous report cannot satisfy a fresh request");
  std::filesystem::remove(request); // Runtime consumes the request before writing.
  std::ofstream(output).close();
  if (report.ready()) throw std::runtime_error("An empty response is not complete");
  std::ofstream(output) << "{\"activeHash\":\"42\"}";
  std::filesystem::last_write_time(output, originalTime);
  if (!report.ready()) throw std::runtime_error("Identical content and timestamps still form a fresh report");
  report.request();
  if (report.ready()) throw std::runtime_error("A second request must retire the first response");
  std::filesystem::remove(request);
  bool refused = false;
  try { proxy::replay::FreshReport(output, directory / "missing" / "request").request(); }
  catch (const std::runtime_error &) { refused = true; }
  if (!refused) throw std::runtime_error("Request creation failures must be reported");
  std::filesystem::remove(directory);
  std::puts("Fresh diagnostic reports reject stale/empty responses and accept identical replacements");
} catch (const std::exception &error) {
  std::fprintf(stderr, "%s\n", error.what());
  return 1;
}
