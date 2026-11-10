#pragma once
#include <array>
#include <remix/remix_c.h>
#include <vector>

namespace proxy::replay {
// Diagnostic dynamic receivers. They share one material and sphere mesh,
// and are submitted independently of the BSP's frozen static bake capture.
class ProbeReceivers {
public:
  ProbeReceivers(remixapi_Interface &api, const std::vector<std::array<float, 4>> &positions,
                 float roughness = 1, float metallic = 0, uint32_t segments = 32);
  ~ProbeReceivers();
  ProbeReceivers(const ProbeReceivers &) = delete;
  ProbeReceivers &operator=(const ProbeReceivers &) = delete;
  // A camera makes positions forward/right/up offsets instead of world XYZ.
  void draw(const remixapi_CameraInfoParameterizedEXT *camera = nullptr);
private:
  void release();
  remixapi_Interface &api;
  std::vector<std::array<float, 4>> positions;
  remixapi_MaterialHandle material = nullptr;
  remixapi_MeshHandle mesh = nullptr;
};
}
