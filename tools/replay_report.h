#pragma once
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace proxy::replay {
// The relay owns its diagnostic directory. Retire the prior response before
// requesting another; identical reports and coarse timestamps are still fresh.
class FreshReport {
public:
  FreshReport(std::filesystem::path output, std::filesystem::path request)
      : output_(std::move(output)), request_(std::move(request)) {}

  void request() const {
    std::error_code error;
    std::filesystem::remove(output_, error);
    if (error) throw std::runtime_error("Cannot retire previous diagnostic report: " + error.message());
    std::ofstream request(request_, std::ios::trunc);
    if (!request) throw std::runtime_error("Cannot request fresh diagnostic report");
  }

  bool ready() const {
    std::error_code error;
    const auto size = std::filesystem::file_size(output_, error);
    return !error && size > 0;
  }

private:
  std::filesystem::path output_, request_;
};
} // namespace proxy::replay
