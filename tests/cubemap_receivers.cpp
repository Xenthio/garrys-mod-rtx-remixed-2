#include "cubemap_receivers.h"
#include <cstdio>
#include <limits>
#include <stdexcept>
using namespace proxy::cubemaps;
namespace {
  void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
  uint32_t reportedVersion = 0;
  uint32_t REMIXAPI_CALL version() { return reportedVersion; }
}
int main() try {
  require(!supportsReceiverBindings(nullptr) && !supportsReceiverBindings(version),
          "Missing and unsupported runtime contracts cannot silently activate receiver bindings");
  reportedVersion = REMIXAPI_REFLECTION_BINDING_VERSION_EXT;
  require(supportsReceiverBindings(version), "Current frame-safe binding contract is accepted");
  ++reportedVersion;
  require(supportsReceiverBindings(version), "Compatible future binding contracts remain accepted");
  const auto origin = captureOrigin("MAPS\\BM_C1A1B\\c-1160_3124_384", "bm_c1a1b");
  require(origin && *origin == CaptureOrigin{-1160, 3124, 384}, "Explicit capture names retain signed coordinates");
  require(captureOrigin("maps//test/c0_0_0", "TEST") == CaptureOrigin{0, 0, 0},
          "Source path aliases normalize without changing capture identity");
  require(!captureOrigin("maps/../c1_2_3", "..") && !captureOrigin("maps/./c1_2_3", "."),
          "Invalid map paths cannot masquerade as local capture references");
  for (const char *invalid : {"env_cubemap", "maps/other/c1_2_3", "maps/test/material_1_2_3",
          "maps/test/c1_2", "maps/test/c1_2_3_4", "maps/test/c1_2_3.vtf", "maps/test/c1_2_3junk",
          "maps/test/c1.5_2_3", "maps/test/c+1_2_3", "maps/test/c2147483648_2_3",
          "maps/test/c-2147483649_2_3", "maps/test/c_2_3", "maps/test/c1__3"})
    require(!captureOrigin(invalid, "test"), "Malformed/nonlocal names must not become capture assignments");
  Document document;
  remixapi_ReflectionVolumeEXT volume{};
  volume.id = UINT64_MAX;
  volume.capture[0] = -1160; volume.capture[1] = 3124; volume.capture[2] = 384;
  document.volumes.push_back(volume);
  require(uniqueVolume(*origin, document) == UINT64_MAX, "Exact capture match preserves full authored ID");
  require(!uniqueVolume({-1159, 3124, 384}, document), "Nearby captures are not identity matches");
  volume.id = 7;
  document.volumes.push_back(volume);
  require(!uniqueVolume(*origin, document), "Coincident volumes remain ambiguous regardless of priority/order");
  document.volumes.resize(1);
  document.volumes[0].capture[0] = 16777216;
  require(!uniqueVolume({16777217, 3124, 384}, document), "Integer capture names cannot alias rounded float coordinates");
  document.volumes[0].capture[0] = std::numeric_limits<float>::quiet_NaN();
  require(!uniqueVolume(*origin, document), "Nonfinite authored captures never match");
  std::puts("Explicit material capture references and unique authored identity matching passed");
} catch (const std::exception &error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
