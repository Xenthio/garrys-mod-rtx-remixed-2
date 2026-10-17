#pragma once
#include <array>
#include <optional>
#include <remix/remix_c.h>
#include <string>

namespace proxy::replay {
struct Settings {
  std::optional<std::array<float, 3>> position, angles;
  float fov = 65;
  uint32_t width = 640, height = 360;
  uint32_t measureFrames = 0, warmupFrames = 120;
  float warmupSeconds = 3;
  bool waitForBake = true;
  bool freezeAnimation = false; // explicit frame-zero material snapshots, not engine playback
  std::string mods; // optional USD mod directory; relative to the settings file
};
// Applies runtime keys through the SDK and parses the replay-only settings.
Settings configure(remixapi_Interface &api, const std::string &path);
remixapi_CameraInfoParameterizedEXT camera(const Settings &settings, const std::array<float, 3> &spawnEye,
                                           const std::array<float, 3> &spawnAngles);
} // namespace proxy::replay
