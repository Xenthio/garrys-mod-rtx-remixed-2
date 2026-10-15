#pragma once
// Standalone dxso: the render-state push block the compiled shaders read
// (fog, alpha test, point size), as DXVK's d3d9_state.h lays it out.
#include <array>
#include <cstdint>
#include "d3d9_util.h"

namespace dxvk {

  struct D3D9RenderStateInfo {
    std::array<float, 3> fogColor = { };
    float fogScale   = 0.0f;
    float fogEnd     = 1.0f;
    float fogDensity = 1.0f;

    uint32_t alphaRef = 0u;

    float pointSize    = 1.0f;
    float pointSizeMin = 1.0f;
    float pointSizeMax = 64.0f;
    float pointScaleA  = 1.0f;
    float pointScaleB  = 0.0f;
    float pointScaleC  = 0.0f;
  };

  enum class D3D9RenderStateItem {
    FogColor   = 0,
    FogScale   = 1,
    FogEnd,
    FogDensity,
    AlphaRef,

    PointSize,
    PointSizeMin,
    PointSizeMax,
    PointScaleA,
    PointScaleB,
    PointScaleC,

    Count
  };

  enum D3D9SharedPSStages {
    D3D9SharedPSStages_Constant,
    D3D9SharedPSStages_BumpEnvMat0,
    D3D9SharedPSStages_BumpEnvMat1,
    D3D9SharedPSStages_BumpEnvLScale,
    D3D9SharedPSStages_BumpEnvLOffset,
    D3D9SharedPSStages_Count,
  };

}
