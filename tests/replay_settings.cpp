#include "replay_settings.h"
#include "replay_readiness.h"
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
  remixapi_PreparationStatisticsEXT preparation{};
  preparation.structSize = sizeof(preparation);
  require(proxy::replay::preparationReady(preparation), "Idle renderer preparation is ready");
  preparation.pending = 1;
  require(!proxy::replay::preparationReady(preparation), "Active backend does not bypass pending preparation");
  preparation.pending = 0; preparation.loading = 1;
  require(!proxy::replay::preparationReady(preparation), "Loading renderer is not capture-ready");
  preparation.loading = 0; preparation.pipelinesRequested = 35; preparation.pipelinesCompleted = 29;
  require(!proxy::replay::preparationReady(preparation), "Unfinished scene variants hold reference warmup");
  preparation.pipelinesCompleted = 35;
  require(proxy::replay::preparationReady(preparation), "Completed scene variants release warmup");
  auto rejectedPreparation = [&] {
    try { proxy::replay::preparationReady(preparation); } catch (const std::runtime_error &) { return; }
    require(false, "Failed or incompatible preparation must reject capture");
  };
  preparation.failed = 1; rejectedPreparation(); preparation.failed = 0;
  preparation.pipelinesFailed = 1; rejectedPreparation(); preparation.pipelinesFailed = 0;
  preparation.structSize = 0; rejectedPreparation();
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
  const auto pathSettings = parse("replay.cameraKey=0 0 0 0 0 350 0\n"
                                  "replay.cameraKey=64 64 32 16 0 10 0\n");
  const auto midpoint = proxy::replay::pathCamera(pathSettings, 32);
  require(midpoint.position.x == 32 && midpoint.position.y == 16 && midpoint.position.z == 8 &&
              std::abs(midpoint.forward.x - 1) < 1e-5f && std::abs(midpoint.forward.y) < 1e-5f,
          "Camera paths interpolate positions and take the short angular route across 360 degrees");
  require(proxy::replay::pathCamera(pathSettings, 1000).position.x == 64 &&
              parse("").cameraPath.empty(), "Camera paths clamp at their endpoint and default off");
  const auto fixedPath = parse("replay.cameraKey=0 -2800 -4200 -64 0 45 0\n"
                               "replay.cameraKey=420 -2800 -4200 -64 0 45 0\n");
  for (uint32_t frame = 0; frame <= 420; ++frame) {
    const auto position = proxy::replay::pathCamera(fixedPath, frame).position;
    require(position.x == -2800 && position.y == -4200 && position.z == -64,
            "Identical camera keys keep a bit-exact position on every frame");
  }
  require(pathSettings.cameraCaptureInterval == 32 && proxy::replay::capturePathFrame(pathSettings, 0) &&
              proxy::replay::capturePathFrame(pathSettings, 32) && proxy::replay::capturePathFrame(pathSettings, 64) &&
              !proxy::replay::capturePathFrame(pathSettings, 31), "Default camera capture cadence is preserved");
  const auto dense = parse("replay.cameraKey=0 0 0 0 0 0 0\nreplay.cameraKey=17 1 0 0 0 0 0\n"
                           "replay.cameraCaptureInterval=4");
  require(proxy::replay::capturePathFrame(dense, 16) && proxy::replay::capturePathFrame(dense, 17) &&
              !proxy::replay::capturePathFrame(dense, 18), "Unaligned path endpoints are captured once, never past the path");
  require(parse("replay.cameraKey=0 0 0 0 0 0 0\nreplay.cameraKey=63 1 0 0 0 0 0\n"
                "replay.cameraCaptureInterval=1").cameraCaptureInterval == 1,
          "Dense capture accepts the exact 64-image boundary");
  refuse("replay.cameraKey=0 0 0 0 0 0 0\nreplay.cameraKey=64 1 0 0 0 0 0\nreplay.cameraCaptureInterval=1");
  for (const auto *value : {"0", "1025", "1.5", "nan", "-1"}) {
    const auto text = std::string("replay.cameraCaptureInterval=") + value;
    refuse(text.c_str());
  }
  refuse("replay.cameraKey=1 0 0 0 0 0 0");
  refuse("replay.cameraKey=0 0 0 0 0 0 0");
  refuse("replay.cameraKey=0 0 0 0 0 0 0\nreplay.cameraKey=0 1 0 0 0 0 0");
  refuse("replay.cameraKey=0 0 0 0 0 0 0\nreplay.cameraKey=1.5 1 0 0 0 0 0");
  refuse("replay.cameraKey=0 0 0 0 0 0 0\nreplay.cameraKey=1025 1 0 0 0 0 0");
  refuse("replay.cameraKey=0 0 0 0 0 0 0\nreplay.cameraKey=32 nan 0 0 0 0 0");
  refuse("replay.cameraKey=0 0 0 0 0 0 0\nreplay.cameraKey=32 1 0 0 0 0 0\nreplay.measureFrames=10");
  refuse("replay.cameraKey=0 0 0 0 0 0 0\nreplay.cameraKey=32 1 0 0 0 0 0\nreplay.waitForBake=0");
  const auto receivers = parse("replay.probeReceiver=1 2 3 16\nreplay.probeReceiver=4 5 6 8\n");
  require(receivers.probeReceivers.size() == 2 && receivers.probeReceivers[1][3] == 8 &&
              parse("").probeReceivers.empty(),
          "Dynamic probe receiver fixtures are explicit, ordered and absent by default");
  refuse("replay.probeReceiver=0 0 0 0");
  refuse("replay.probeReceiver=0 0 0 4097");
  refuse("replay.probeReceiver=0 0 nan 16");
  const auto reflective = parse("replay.receiverRoughness=.15\nreplay.receiverMetallic=1\nreplay.receiversFollowCamera=1");
  require(reflective.receiverRoughness == .15f && reflective.receiverMetallic == 1 &&
              reflective.receiversFollowCamera && !parse("").receiversFollowCamera &&
              parse("").receiverRoughness == 1 && parse("").receiverMetallic == 0,
          "Reflective moving receivers are explicit; existing matte world fixtures remain the default");
  refuse("replay.receiverRoughness=nan");
  refuse("replay.receiverRoughness=-.1");
  refuse("replay.receiverMetallic=1.1");
  refuse("replay.receiversFollowCamera=2");
  require(parse("").receiverSegments == 32 && parse("replay.receiverSegments=128").receiverSegments == 128 &&
              parse("replay.receiverSegments=8").receiverSegments == 8 &&
              parse("replay.receiverSegments=256").receiverSegments == 256,
          "Reference sphere detail is explicit and bounded while old fixtures retain their geometry");
  for (const auto *value : {"7", "257", "33", "nan", "16.5", "-8", "128 extra"}) {
    const auto text = std::string("replay.receiverSegments=") + value;
    refuse(text.c_str());
  }
  refuse("replay.fov=179");
  refuse("replay.unknown=1");
  require(!proxy::replay::Settings{}.freezeAnimation &&
              parse("replay.freezeAnimation=1\n").freezeAnimation &&
              !parse("replay.freezeAnimation=0\n").freezeAnimation,
          "Frozen animation snapshots require an explicit diagnostic setting");
  refuse("replay.freezeAnimation=2");
  refuse("replay.freezeAnimation=nan");
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
  require(!parse("").requireSky && parse("replay.requireSky=1").requireSky &&
              !parse("replay.requireSky=0").requireSky,
          "Quality sky requirements are explicit and stay in the replay host");
  refuse("replay.requireSky=2");
  refuse("replay.requireSky=-1");
  require(parse("replay.denoiseFrameDelayMs=50").denoiseFrameDelayMs == 50 &&
              parse("replay.denoiseFrameDelayMs=1000").denoiseFrameDelayMs == 1000 &&
              parse("").denoiseFrameDelayMs == 0,
          "Denoiser contention experiments have an explicit default-off bounded frame delay");
  refuse("replay.denoiseFrameDelayMs=1001");
  refuse("replay.denoiseFrameDelayMs=-1");
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
  require(parse("replay.styledLights=1\nreplay.lightStyleValue=0").styledLights &&
          parse("replay.lightStyleValue=0.5").lightStyleValue == .5f,
          "Diagnostic style controls permit initially-off and partial-intensity map snapshots");
  refuse("replay.styledLights=2");
  refuse("replay.lightStyleValue=-1");
  refuse("replay.lightStyleValue=nan");
  refuse("replay.lightStyleValue=5");
  const auto stylePath = parse("replay.styledLights=1\nreplay.cameraKey=0 0 0 0 0 0 0\n"
                               "replay.cameraKey=48 0 0 0 0 0 0\n"
                               "replay.lightStyleKey=16 0\nreplay.lightStyleKey=32 1 34");
  require(stylePath.lightStyleKeys.size() == 2 && stylePath.lightStyleKeys[0].frame == 16 &&
          stylePath.lightStyleKeys[0].multiplier == 0 && stylePath.lightStyleKeys[0].style == 0 &&
          stylePath.lightStyleKeys[1].multiplier == 1 && stylePath.lightStyleKeys[1].style == 34,
          "Lightstyle steps are tied to camera path frames");
  refuse("replay.styledLights=1\nreplay.lightStyleKey=16 0");
  refuse("replay.styledLights=1\nreplay.cameraKey=0 0 0 0 0 0 0\n"
         "replay.cameraKey=48 0 0 0 0 0 0\nreplay.lightStyleKey=16 0 128");
  refuse("replay.styledLights=1\nreplay.cameraKey=0 0 0 0 0 0 0\n"
         "replay.cameraKey=48 0 0 0 0 0 0\nreplay.lightStyleKey=49 0");
  refuse("replay.lightStyleKey=16 0\nreplay.lightStyleKey=16 1");
  require(!parse("").probeBounds &&
              parse("replay.probeBounds=-4 -3 -2 4 3 2").probeBounds ==
                  std::optional<std::array<float, 6>>({-4, -3, -2, 4, 3, 2}),
          "Local reference probe bounds are explicit and default off");
  refuse("replay.probeBounds=0 0 0 0 1 1");
  refuse("replay.probeBounds=0 0 0 1 -1 1");
  refuse("replay.probeBounds=0 0 0 1 1 nan");
  refuse("replay.probeBounds=0 0 0 1 1");
  refuse("replay.probeBounds=0 0 0 1 1 1 extra");
  std::filesystem::remove(path);
  std::puts("Replay viewpoint parsing, Source camera basis and SDK delegation passed");
}
