#include "refract_native.h"
#include <remix/remix_native_ext.h>
#include <remix/remix_bake_ext.h>
#include <algorithm>
#include <cmath>

namespace proxy::refract {

remixapi_ErrorCode create(PFN_remixapi_CreateMaterial createMaterial, const Description &description,
                         const Resources &resources, remixapi_MaterialHandle &output, std::string &reason) {
  auto invalid = [&](const char *message) {
    reason = message;
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  };
  if (!createMaterial || !resources.hash)
    return invalid("Refract requires a runtime and material identity");
  if ((!description.normalTexture.empty() && !resources.normal) ||
      (!description.tintTexture.empty() && !resources.tint))
    return invalid("Refract texture dependency is not loaded");
  if (resources.tintFrames != 1)
    return invalid("Animated Refract tint images are not supported by stationary baking");
  if (!resources.normalFrames || !resources.proxyFrames ||
      (!description.animatedTexture.empty() && resources.proxyFrames != resources.normalFrames))
    return invalid("Refract bump-frame proxy and normal image frame counts disagree");
  if (!std::isfinite(description.framesPerSecond) || description.framesPerSecond < 0 ||
      !std::all_of(description.scroll.begin(), description.scroll.end(), [](float v) { return std::isfinite(v); }) ||
      !std::all_of(description.tint.begin(), description.tint.end(), [](float v) { return std::isfinite(v) && v >= 0; }))
    return invalid("Refract optical parameters must be finite");

  remixapi_MaterialNormalEncodingEXT encoding{};
  encoding.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_NORMAL_ENCODING_EXT;
  encoding.pNext = resources.replacementChain;
  encoding.encoding = 5;
  remixapi_MaterialNormalScrollEXT normalScroll{};
  normalScroll.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_NORMAL_SCROLL_EXT;
  normalScroll.pNext = &encoding;
  normalScroll.layers = 1;
  std::copy(description.scroll.begin(), description.scroll.end(), normalScroll.base);

  remixapi_MaterialInfoTranslucentEXT sheet{};
  sheet.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
  sheet.pNext = resources.normal ? static_cast<void *>(&normalScroll) : resources.replacementChain;
  sheet.transmittanceTexture = resources.tint;
  sheet.refractiveIndex = 1.5f;
  // One virtual Source unit for both lengths gives the authored transmittance
  // at normal incidence. Screen-space $refractamount is intentionally unrelated.
  sheet.transmittanceMeasurementDistance = 1;
  sheet.thinWallThickness_hasvalue = 1;
  sheet.thinWallThickness_value = 1;
  sheet.transmittanceColor = resources.tint ? remixapi_Float3D{1, 1, 1}
      : remixapi_Float3D{std::min(1.f, description.tint[0]), std::min(1.f, description.tint[1]),
                        std::min(1.f, description.tint[2])};

  remixapi_MaterialBakeBoundaryEXT boundary{};
  boundary.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_BAKE_BOUNDARY_EXT;
  boundary.pNext = &sheet;
  boundary.smoothBoundary = 1;
  remixapi_MaterialTransmittanceScrollEXT tintScroll{};
  tintScroll.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_TRANSMITTANCE_SCROLL_EXT;
  tintScroll.pNext = &boundary;
  remixapi_MaterialBakeTintAverageEXT average{};
  average.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_BAKE_TINT_AVERAGE_EXT;
  average.pNext = &tintScroll;
  if (resources.tint) {
    for (unsigned axis = 0; axis < 2; ++axis) {
      tintScroll.rate[axis] = description.scroll[axis];
      if (description.scroll[axis] != 0)
        average.axes |= 1u << axis;
    }
  }
  // This is an independent phase distribution per selected UV axis, not a
  // claim that two scrolling coordinates share a coherent animation period.
  remixapi_MaterialInfo info{};
  info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
  info.pNext = &average;
  info.hash = resources.hash;
  info.normalTexture = resources.normal;
  info.filterMode = 1;
  info.wrapModeU = info.wrapModeV = 1;
  remixapi_MaterialHandle handle = nullptr;
  const auto status = createMaterial(&info, &handle);
  if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
    reason = "Runtime rejected the Refract optical sheet";
    return status;
  }
  output = handle;
  reason.clear();
  return status;
}
} // namespace proxy::refract
