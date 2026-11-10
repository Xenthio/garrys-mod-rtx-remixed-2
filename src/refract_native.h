#pragma once

#include "refract_material.h"
#include <remix/remix_c.h>

namespace proxy::refract {

// Resource ownership stays with the BSP scene. Paths and replacement-chain
// storage only need to survive the synchronous CreateMaterial call.
struct Resources {
  uint64_t hash = 0;
  remixapi_Path normal = nullptr;
  remixapi_Path tint = nullptr;
  void *replacementChain = nullptr;
  uint32_t normalFrames = 1;
  uint32_t proxyFrames = 1;
  uint32_t tintFrames = 1;
};

remixapi_ErrorCode create(PFN_remixapi_CreateMaterial createMaterial, const Description &description,
                         const Resources &resources, remixapi_MaterialHandle &output, std::string &reason);

} // namespace proxy::refract
