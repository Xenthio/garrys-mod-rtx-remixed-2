#pragma once
// Standalone dxso: of DXVK's d3d9_util.h, the sampler slot layout.
#include "d3d9_caps.h"
namespace dxvk {
  constexpr uint32_t FirstVSSamplerSlot = caps::MaxTexturesPS + 1;
}
