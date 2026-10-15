#pragma once
// Standalone dxso: the shader-related options DXVK reads from its config.
#include <cstdint>

namespace dxvk {

  enum class D3D9FloatEmulation {
    Disabled,
    Enabled,
    Strict
  };

  struct D3D9Options {
    bool strictConstantCopies = false;
    bool strictPow = true;
    D3D9FloatEmulation d3d9FloatEmulation = D3D9FloatEmulation::Enabled;
    bool invariantPosition = true;
    bool forceSamplerTypeSpecConstants = false;
    bool forceSampleRateShading = false;
    bool sincosEmulation = false;
    int32_t drefScaling = 0;
  };

}
