#include "replay_settings.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {
void require(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
  }
}
std::string applied;
remixapi_ErrorCode REMIXAPI_CALL variable(const char *key, const char *value) {
  applied = std::string(key) + '=' + value;
  return std::string(key) == "bad" ? REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS : REMIXAPI_ERROR_CODE_SUCCESS;
}
float dot(remixapi_Float3D a, remixapi_Float3D b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
} // namespace
int main() {
  remixapi_Interface api{};
  api.SetConfigVariable = variable;
  const auto path = std::filesystem::path("replay-settings-test.cfg");
  auto parse = [&](const char *text) {
    {
      std::ofstream file(path);
      file << text;
    }
    return proxy::replay::configure(api, path.string());
  };
  auto refuse = [&](const char *text) {
    bool refused = false;
    try {
      parse(text);
    } catch (const std::runtime_error &) {
      refused = true;
    }
    require(refused, "Malformed camera/runtime settings must be refused");
  };
  const auto defaults = proxy::replay::camera({}, {10, 20, 30}, {0, 0, 0});
  require(defaults.position.z == 30 && defaults.forward.x == 1 && defaults.right.y == -1 &&
              defaults.up.z == 1,
          "Default Source spawn orientation is preserved");
  const auto settings = parse("# inspection view\nreplay.position = 1 2 3\nreplay.angles = 30 45 "
                              "90\nreplay.fov=80\nrtx.native.forward.bakeBatch = 1024\n");
  const auto camera = proxy::replay::camera(settings, {}, {});
  require(camera.position.x == 1 && camera.position.z == 3 && camera.fovYInDegrees == 80 &&
              applied == "rtx.native.forward.bakeBatch=1024",
          "Camera keys stay local; runtime settings reach the SDK");
  require(std::abs(dot(camera.forward, camera.up)) < 1e-5f &&
              std::abs(dot(camera.forward, camera.right)) < 1e-5f &&
              std::abs(dot(camera.right, camera.up)) < 1e-5f &&
              std::abs(dot(camera.right, camera.right) - 1) < 1e-5f,
          "Pitch, yaw and roll retain an orthonormal camera basis");
  refuse("replay.position=1 2 3 4");
  refuse("replay.position=nan 0 0");
  refuse("replay.fov=179");
  refuse("replay.unknown=1");
  const auto mods = parse("replay.mods=materials with spaces\n");
  require(mods.mods == "materials with spaces", "Replay mod paths preserve spaces without reaching runtime settings");
  refuse("replay.mods=\n");
  refuse("bad=1");
  refuse("missing equal");
  const auto measurement = parse("replay.measureFrames=240\nreplay.warmupFrames=64\nreplay.waitForBake=0\n");
  require(measurement.measureFrames == 240 && measurement.warmupFrames == 64 && !measurement.waitForBake,
          "Measurement settings stay in the replay host");
  refuse("replay.measureFrames=-1");
  refuse("replay.measureFrames=10.5");
  refuse("replay.measureFrames=10001");
  refuse("replay.warmupFrames=0");
  refuse("replay.waitForBake=2");
  require(parse("replay.warmupSeconds=2.5").warmupSeconds == 2.5f, "Timed GPU warmup is configurable");
  refuse("replay.warmupSeconds=-1");
  refuse("replay.warmupSeconds=61");
  const auto dimensions = parse("replay.width=1440\nreplay.height=1080");
  require(dimensions.width == 1440 && dimensions.height == 1080 &&
              std::abs(proxy::replay::camera(dimensions, {}, {}).aspect - 4.f / 3) < 1e-6f,
          "Replay resolution determines the camera aspect ratio");
  refuse("replay.width=0");
  refuse("replay.height=-360");
  refuse("replay.width=4097");
  refuse("replay.height=1080.5");
  refuse("replay.width=1920junk");
  std::filesystem::remove(path);
  std::puts("Replay viewpoint parsing, Source camera basis and SDK delegation passed");
}
