#pragma once
#include <cstdint>
#include <remix/remix_c.h>

namespace proxy::lighting {
// Types 1..3 and this 76-byte prefix match Source's 88-byte LightDesc_t.
// The bridge skips its unused trailing fields when advancing engine arrays.
// Surface is an internal BSP tag; it never comes from the engine ABI.
constexpr int kSurface = 4;
struct SourceLight {
  int32_t type;
  float color[3], position[3], direction[3];
  float range, falloff, attenuation0, attenuation1, attenuation2, theta, phi, thetaDot, phiDot;
};
static_assert(sizeof(SourceLight) == 76);

// Shared by the engine bridge and the standalone map host. Positions, emitter
// radius and attenuation distances all remain in Source units.
remixapi_LightHandle createLight(remixapi_Interface &api, const SourceLight &light, uint64_t hash,
                                float lightScale = 1, float sunScale = 1);
} // namespace proxy::lighting
