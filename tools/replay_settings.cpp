#include "replay_settings.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
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
      if (key == "replay.mods") {
        if (value.empty()) throw std::runtime_error("USD mod directory must not be empty");
        auto directory = std::filesystem::u8path(value);
        if (directory.is_relative()) directory = std::filesystem::u8path(path).parent_path() / directory;
        settings.mods = directory.lexically_normal().u8string();
      } else if (key == "replay.position")
        settings.position = numbers<3>(value);
      else if (key == "replay.angles")
        settings.angles = numbers<3>(value);
      else if (key == "replay.cameraKey") {
        const auto values = numbers<7>(value);
        const float frame = values[0];
        if (frame < 0 || frame > 1024 || frame != std::floor(frame) ||
            settings.cameraPath.size() >= 64 ||
            (settings.cameraPath.empty() ? frame != 0 : frame <= settings.cameraPath.back().frame))
          throw std::runtime_error("Camera keys need increasing integer frames in [0,1024], starting at zero; at most 64 keys");
        settings.cameraPath.push_back({uint32_t(frame), {values[1], values[2], values[3]},
                                      {values[4], values[5], values[6]}});
      }
      else if (key == "replay.cameraCaptureInterval") {
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), settings.cameraCaptureInterval);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
            !settings.cameraCaptureInterval || settings.cameraCaptureInterval > 1024)
          throw std::runtime_error("Camera capture interval must be an integer in [1,1024]");
      }
      else if (key == "replay.probeReceiver") {
        const auto receiver = numbers<4>(value);
        if (receiver[3] <= 0 || receiver[3] > 4096 || settings.probeReceivers.size() >= 64)
          throw std::runtime_error("Probe receivers require a radius in (0,4096] and at most 64 entries");
        settings.probeReceivers.push_back(receiver);
      }
      else if (key == "replay.receiverSegments") {
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), settings.receiverSegments);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
            settings.receiverSegments < 8 || settings.receiverSegments > 256 || settings.receiverSegments % 2)
          throw std::runtime_error("Receiver segments must be an even integer in [8,256]");
      }
      else if (key == "replay.receiverRoughness" || key == "replay.receiverMetallic") {
        const float scalar = numbers<1>(value)[0];
        if (scalar < 0 || scalar > 1)
          throw std::runtime_error("Receiver material scalars must be in [0,1]");
        if (key == "replay.receiverRoughness") settings.receiverRoughness = scalar;
        else settings.receiverMetallic = scalar;
      }
      else if (key == "replay.probeBounds") {
        const auto bounds = numbers<6>(value);
        for (size_t axis = 0; axis < 3; ++axis)
          if (bounds[axis] >= bounds[axis + 3])
            throw std::runtime_error("Probe bounds must have positive extent on every axis");
        settings.probeBounds = bounds;
      }
      else if (key == "replay.fov") {
        settings.fov = numbers<1>(value)[0];
        if (settings.fov < 1 || settings.fov >= 179)
          throw std::runtime_error("Camera FOV must be in [1,179)");
      } else if (key == "replay.lightStyleValue") {
        settings.lightStyleValue = numbers<1>(value)[0];
        if (settings.lightStyleValue < 0 || settings.lightStyleValue > 4)
          throw std::runtime_error("Diagnostic style value must be in [0,4]");
      } else if (key == "replay.lightStyleKey") {
        std::istringstream input(value);
        float frame, multiplier;
        if (!(input >> frame >> multiplier) || !std::isfinite(frame) || !std::isfinite(multiplier))
          throw std::runtime_error("Lightstyle key requires a finite frame and multiplier");
        uint32_t style = 0;
        if (!(input >> std::ws).eof()) {
          if (!(input >> style) || style == 0 || style > 127 || !(input >> std::ws).eof())
            throw std::runtime_error("Lightstyle key group must be an integer in [1,127]");
        }
        if (frame < 0 || frame > 1024 || frame != std::floor(frame) ||
            (!settings.lightStyleKeys.empty() && frame <= settings.lightStyleKeys.back().frame))
          throw std::runtime_error("Lightstyle keys need increasing integer frames in [0,1024]");
        if (multiplier < 0 || multiplier > 4)
          throw std::runtime_error("Diagnostic style value must be in [0,4]");
        settings.lightStyleKeys.push_back({uint32_t(frame), multiplier, style});
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
                 key == "replay.waitForBake" || key == "replay.freezeAnimation" || key == "replay.styledLights" ||
                 key == "replay.receiversFollowCamera" || key == "replay.requireSky" ||
                 key == "replay.denoiseFrameDelayMs") {
        uint32_t number = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
        const uint32_t maximum = key == "replay.denoiseFrameDelayMs" ? 1000 :
            key == "replay.waitForBake" || key == "replay.freezeAnimation" || key == "replay.styledLights" ||
            key == "replay.receiversFollowCamera" || key == "replay.requireSky" ? 1 : 10000;
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || number > maximum ||
            (key == "replay.warmupFrames" && number < 16))
          throw std::runtime_error("Invalid measurement setting: " + key);
        if (key == "replay.measureFrames")
          settings.measureFrames = number;
        else if (key == "replay.warmupFrames")
          settings.warmupFrames = number;
        else if (key == "replay.waitForBake")
          settings.waitForBake = number != 0;
        else if (key == "replay.requireSky")
          settings.requireSky = number != 0;
        else if (key == "replay.styledLights")
          settings.styledLights = number != 0;
        else if (key == "replay.receiversFollowCamera")
          settings.receiversFollowCamera = number != 0;
        else if (key == "replay.denoiseFrameDelayMs")
          settings.denoiseFrameDelayMs = number;
        else
          settings.freezeAnimation = number != 0;
      } else if (key.rfind("replay.", 0) == 0)
        throw std::runtime_error("Unknown replay setting: " + key);
      else if (api.SetConfigVariable(key.c_str(), value.c_str()) != REMIXAPI_ERROR_CODE_SUCCESS)
        throw std::runtime_error("Runtime refused setting: " + key);
    } catch (const std::exception &error) {
      throw std::runtime_error("Replay settings line " + std::to_string(number) + ": " + error.what());
    }
  }
  if (!settings.cameraPath.empty() &&
      (settings.cameraPath.size() < 2 || settings.measureFrames || !settings.waitForBake))
    throw std::runtime_error("Camera paths require two or more keys, bake readiness, and no fixed-view timing window");
  if (!settings.cameraPath.empty()) {
    const auto last = settings.cameraPath.back().frame, interval = settings.cameraCaptureInterval;
    const auto count = last / interval + 1 + uint32_t(last % interval != 0);
    if (count > 64) throw std::runtime_error("Camera path exceeds 64 screenshots; increase its capture interval");
  }
  if (!settings.lightStyleKeys.empty() && (!settings.styledLights || settings.cameraPath.empty() ||
      settings.lightStyleKeys.back().frame > settings.cameraPath.back().frame))
    throw std::runtime_error("Lightstyle keys require styled lights and must fall within the camera path");
  return settings;
}
bool capturePathFrame(const Settings &settings, uint32_t frame) {
  return !settings.cameraPath.empty() && settings.cameraCaptureInterval &&
         frame <= settings.cameraPath.back().frame &&
         (frame % settings.cameraCaptureInterval == 0 || frame == settings.cameraPath.back().frame);
}
remixapi_CameraInfoParameterizedEXT pathCamera(const Settings &settings, uint32_t frame) {
  if (settings.cameraPath.empty())
    throw std::invalid_argument("Camera path is empty");
  Settings view;
  view.width = settings.width;
  view.height = settings.height;
  view.fov = settings.fov;
  const auto next = std::upper_bound(settings.cameraPath.begin(), settings.cameraPath.end(), frame,
      [](uint32_t value, const CameraKey &key) { return value < key.frame; });
  const auto &a = next == settings.cameraPath.begin() ? *next : *(next - 1);
  const auto &b = next == settings.cameraPath.end() ? a : *next;
  const float t = a.frame == b.frame ? 0 : float(frame - a.frame) / float(b.frame - a.frame);
  view.position = a.position;
  view.angles = a.angles;
  for (size_t axis = 0; axis < 3; ++axis) {
    // A stationary axis must stay exact, including during light-only replays.
    if (a.position[axis] != b.position[axis])
      (*view.position)[axis] = a.position[axis] * (1 - t) + b.position[axis] * t;
    (*view.angles)[axis] += t * std::remainder(b.angles[axis] - a.angles[axis], 360.f);
  }
  return camera(view, {}, {});
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
