#pragma once
// Standalone dxso (the GMod OpenRemix proxy): of DXVK's d3d9_fixed_function.h,
// the helpers the shader compiler calls (fog, alpha test, point size, the
// render-state block, the spec-constant UBO); the fixed-function shader
// generators are left out.
#include "d3d9_include.h"
#include "d3d9_caps.h"

#include "../dxvk/dxvk_shader.h"
#include "../dxso/dxso_isgn.h"

#include <vector>

namespace dxvk {

  class D3D9DeviceEx;
  class SpirvModule;
  struct D3D9Options;
  class D3D9ShaderSpecConstantManager;

  struct D3D9FogContext {
    bool     IsPixel;
    bool     RangeFog;
    uint32_t RenderState;
    uint32_t vPos;
    uint32_t vFog;
    uint32_t oColor;
    bool     HasFogInput;
    bool     IsFixedFunction;
    bool     IsPositionT;
    bool     HasSpecular;
    uint32_t Specular;
    uint32_t SpecUBO;
  };

  struct D3D9AlphaTestContext {
    uint32_t alphaId;
    uint32_t alphaPrecisionId;
    uint32_t alphaFuncId;
    uint32_t alphaRefId;
  };

  constexpr float GetDrefScaleFactor(int32_t bitDepth) {
    return 1.0f / (float(1 << bitDepth) - 1.0f);
  }

  uint32_t DoFixedFunctionFog(D3D9ShaderSpecConstantManager& spec, SpirvModule& spvModule, const D3D9FogContext& fogCtx);
  void DoFixedFunctionAlphaTest(SpirvModule& spvModule, const D3D9AlphaTestContext& ctx);
  uint32_t SetupRenderStateBlock(SpirvModule& spvModule);

  struct D3D9PointSizeInfoVS {
    uint32_t defaultValue;
    uint32_t min;
    uint32_t max;
  };
  D3D9PointSizeInfoVS GetPointSizeInfoVS(D3D9ShaderSpecConstantManager& spec, SpirvModule& spvModule, uint32_t vPos, uint32_t vtx, uint32_t perVertPointSize, uint32_t rsBlock, uint32_t specUbo, bool isFixedFunction);

  struct D3D9PointSizeInfoPS {
    uint32_t isSprite;
  };
  D3D9PointSizeInfoPS GetPointSizeInfoPS(D3D9ShaderSpecConstantManager& spec, SpirvModule& spvModule, uint32_t rsBlock, uint32_t specUbo);

  uint32_t GetPointCoord(SpirvModule& spvModule);
  uint32_t GetSharedConstants(SpirvModule& spvModule);
  uint32_t SetupSpecUBO(SpirvModule& spvModule, std::vector<DxvkBindingInfo>& bindings);

}
