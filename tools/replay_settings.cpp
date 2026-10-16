#include "replay_settings.h"
#include <charconv>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace proxy::replay {
namespace {
std::string trim(const std::string &value) {
  const auto first = value.find_first_not_of(" \t\r");
  return first == std::string::npos ? "" : value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
}
template <size_t count> std::array<float, count> numbers(const std::string &value) {
  std::istringstream input(value);
  std::array<float, count> result;
  for (auto &number : result)
    if (!(input >> number) || !std::isfinite(number))
      throw std::runtime_error("Camera value must contain " + std::to_string(count) + " finite numbers");
  input >> std::ws;
  if (!input.eof())
    throw std::runtime_error("Unexpected text after camera value");
  return result;
}
} // namespace
Settings configure(remixapi_Interface &api, const std::string &path) {
  std::ifstream file(path);
  if (!file)
    throw std::runtime_error("Cannot read replay settings: " + path);
  Settings settings;
  std::string line;
  unsigned number = 0;
  while (std::getline(file, line)) {
    ++number;
    line = trim(line);
    if (line.empty() || line[0] == '#')
      continue;
    const auto equal = line.find('=');
    if (equal == std::string::npos)
      throw std::runtime_error("Missing '=' in replay settings line " + std::to_string(number));
    const auto key = trim(line.substr(0, equal)), value = trim(line.substr(equal + 1));
    try {
      if (key == "replay.position")
        settings.position = numbers<3>(value);
      else if (key == "replay.angles")
        settings.angles = numbers<3>(value);
      else if (key == "replay.fov") {
        settings.fov = numbers<1>(value)[0];
        if (settings.fov < 1 || settings.fov >= 179)
          throw std::runtime_error("Camera FOV must be in [1,179)");
      } else if (key == "replay.warmupSeconds") {
        settings.warmupSeconds = numbers<1>(value)[0];
        if (settings.warmupSeconds < 0 || settings.warmupSeconds > 60)
          throw std::runtime_error("Warmup seconds must be in [0,60]");
      } else if (key == "replay.width" || key == "replay.height") {
        uint32_t dimension = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), dimension);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || dimension < 64 ||
            dimension > 4096)
          throw std::runtime_error("Replay dimension must be an integer in [64,4096]");
        if (key == "replay.width")
          settings.width = dimension;
        else
          settings.height = dimension;
      } else if (key == "replay.measureFrames" || key == "replay.warmupFrames" ||
                 key == "replay.waitForBake") {
        uint32_t number = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
        const uint32_t maximum = key == "replay.waitForBake" ? 1 : 10000;
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || number > maximum ||
            (key == "replay.warmupFrames" && number < 16))
          throw std::runtime_error("Invalid measurement setting: " + key);
        if (key == "replay.measureFrames")
          settings.measureFrames = number;
        else if (key == "replay.warmupFrames")
          settings.warmupFrames = number;
        else
          settings.waitForBake = number != 0;
      } else if (key.rfind("replay.", 0) == 0)
        throw std::runtime_error("Unknown replay setting: " + key);
      else if (api.SetConfigVariable(key.c_str(), value.c_str()) != REMIXAPI_ERROR_CODE_SUCCESS)
        throw std::runtime_error("Runtime refused setting: " + key);
    } catch (const std::exception &error) {
      throw std::runtime_error("Replay settings line " + std::to_string(number) + ": " + error.what());
    }
  }
  return settings;
}
remixapi_CameraInfoParameterizedEXT camera(const Settings &settings, const std::array<float, 3> &spawnEye,
                                           const std::array<float, 3> &spawnAngles) {
  const auto origin = settings.position.value_or(spawnEye), angles = settings.angles.value_or(spawnAngles);
  constexpr float radians = .01745329252f;
  const float pitch = angles[0] * radians, yaw = angles[1] * radians, roll = angles[2] * radians;
  const std::array<float, 3> right{std::sin(yaw), -std::cos(yaw), 0};
  const std::array<float, 3> up{std::sin(pitch) * std::cos(yaw), std::sin(pitch) * std::sin(yaw),
                                std::cos(pitch)};
  remixapi_CameraInfoParameterizedEXT result{};
  result.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
  result.position = {origin[0], origin[1], origin[2]};
  result.forward = {std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), -std::sin(pitch)};
  result.right = {right[0] * std::cos(roll) - up[0] * std::sin(roll),
                  right[1] * std::cos(roll) - up[1] * std::sin(roll),
                  right[2] * std::cos(roll) - up[2] * std::sin(roll)};
  result.up = {up[0] * std::cos(roll) + right[0] * std::sin(roll),
               up[1] * std::cos(roll) + right[1] * std::sin(roll),
               up[2] * std::cos(roll) + right[2] * std::sin(roll)};
  result.aspect = float(settings.width) / settings.height;
  result.fovYInDegrees = settings.fov;
  result.nearPlane = 2;
  result.farPlane = 100000;
  return result;
}
} // namespace proxy::replay
