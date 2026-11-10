#include "refract_material.h"
#include "refract_native.h"
#include <remix/remix_native_ext.h>
#include <remix/remix_bake_ext.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
unsigned calls = 0;
bool reject = false;
void require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
  }
}

remixapi_ErrorCode REMIXAPI_CALL capture(const remixapi_MaterialInfo *info, remixapi_MaterialHandle *output) {
  ++calls;
  const auto &average = *static_cast<const remixapi_MaterialBakeTintAverageEXT *>(info->pNext);
  require(average.sType == REMIXAPI_STRUCT_TYPE_MATERIAL_BAKE_TINT_AVERAGE_EXT && average.axes == 3,
          "Scrolling tint selects both independent bake axes");
  const auto &scroll = *static_cast<const remixapi_MaterialTransmittanceScrollEXT *>(average.pNext);
  require(scroll.rate[0] != 0 && scroll.rate[1] != 0, "Live optical tint retains motion");
  const auto &boundary = *static_cast<const remixapi_MaterialBakeBoundaryEXT *>(scroll.pNext);
  require(boundary.smoothBoundary == 1, "Animated normals use a stationary bake boundary");
  const auto &sheet = *static_cast<const remixapi_MaterialInfoTranslucentEXT *>(boundary.pNext);
  require(sheet.thinWallThickness_hasvalue && sheet.thinWallThickness_value == 1 &&
          sheet.transmittanceMeasurementDistance == 1 && sheet.refractiveIndex == 1.5f &&
          sheet.transmittanceColor.x == 1, "Physical sheet is independent of Source screen warp");
  const auto &normal = *static_cast<const remixapi_MaterialNormalScrollEXT *>(sheet.pNext);
  require(normal.layers == 1 && normal.base[0] == scroll.rate[0], "Normal and tint scroll together");
  const auto &encoding = *static_cast<const remixapi_MaterialNormalEncodingEXT *>(normal.pNext);
  require(encoding.encoding == 5 && encoding.pNext == reinterpret_cast<void *>(uintptr_t(42)),
          "Original replacement identities survive native extension assembly");
  if (reject)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  *output = reinterpret_cast<remixapi_MaterialHandle>(uintptr_t(123));
  return REMIXAPI_ERROR_CODE_SUCCESS;
}
}

int main() {
  using namespace proxy::refract;
  Values values = {{"$normalmap", "Models/Effects/com_shield001a_normal"},
                   {"$dudvmap", "models/effects/com_shield001a_dudv"},
                   {"$refracttinttexture", "models/effects/com_shield001a"},
                   {"$refractamount", ".05"}, {"$bumpframe", "0"}, {"$nocull", "1"}};
  Values proxies = {{"texturescroll.texturescrollvar", "$bumptransform"},
                    {"texturescroll", ""}, {"animatedtexture", ""},
                    {"texturescroll.texturescrollrate", ".2"},
                    {"texturescroll.texturescrollangle", "80"},
                    {"animatedtexture.animatedtexturevar", "$dudvmap"},
                    {"animatedtexture.animatedtextureframenumvar", "$bumpframe"},
                    {"animatedtexture.animatedtextureframerate", "19"}};
  Description result;
  std::string reason;
  require(describe(values, proxies, result, reason), "Shield proxy binding must be supported");
  require(result.noCull && result.framesPerSecond == 19 &&
          result.normalTexture == "models/effects/com_shield001a_normal" &&
          result.animatedTexture == values["$dudvmap"], "DUDV controls bump frame, not tint playback");
  require(std::fabs(result.scroll[0] - .03472964f) < 1e-6f &&
          std::fabs(result.scroll[1] - .19696155f) < 1e-6f, "Scroll uses Source angle in degrees");
  const auto saved = result;
  Resources resources;
  resources.hash = 123;
  resources.normal = L"0x10";
  resources.tint = L"0x20";
  resources.replacementChain = reinterpret_cast<void *>(uintptr_t(42));
  remixapi_MaterialHandle handle = nullptr;
  require(create(capture, saved, resources, handle, reason) == REMIXAPI_ERROR_CODE_SUCCESS && handle,
          "Shield native material chain is accepted");
  const auto original = handle;
  resources.proxyFrames = 2;
  require(create(capture, saved, resources, handle, reason) != REMIXAPI_ERROR_CODE_SUCCESS && calls == 1,
          "Mismatched proxy cycles cannot reach runtime creation");
  resources.proxyFrames = 1;
  resources.tintFrames = 2;
  require(create(capture, saved, resources, handle, reason) != REMIXAPI_ERROR_CODE_SUCCESS && calls == 1,
          "Animated tint cannot silently freeze its first frame");
  resources.tintFrames = 1;
  reject = true;
  require(create(capture, saved, resources, handle, reason) != REMIXAPI_ERROR_CODE_SUCCESS && handle == original,
          "Runtime rejection preserves the caller's existing material handle");
  for (const auto &invalid : {"nan", "inf", "1 junk", ""}) {
    proxies["texturescroll.texturescrollrate"] = invalid;
    require(!describe(values, proxies, result, reason) && !reason.empty(), "Malformed rates fail closed");
    require(result.scroll == saved.scroll, "Failure cannot publish a partially parsed description");
  }
  proxies.clear();
  values["$refracttint"] = "{128 255 0}";
  require(describe(values, proxies, result, reason) && result.tint[0] == 128.f / 255,
          "Byte tint syntax is normalized");
  values["$refracttint"] = "[1 2 3] trailing";
  require(!describe(values, proxies, result, reason), "Trailing colour input is rejected");
  values.erase("$refracttint");
  proxies["animatedtexture.animatedtextureframerate"] = "19";
  require(!describe(values, proxies, result, reason), "Orphan animation keys cannot be ignored");
  proxies.clear();
  values["$flowmap"] = "effects/flow";
  require(!describe(values, proxies, result, reason), "Unsupported flowmaps need a concrete diagnostic");
  std::vector<float> pixels;
  require(passiveTint({0, 128, 255, 0, 255, 255, 255, 255}, {1, .5f, 2}, pixels),
          "Tint conversion accepts RGBA source");
  require(pixels[0] == 0 && std::fabs(pixels[1] - .2158605f) < 1e-6f &&
          pixels[2] == 1 && pixels[3] == 1 && pixels[4] == 1 && pixels[7] == 1,
          "Tint is sRGB decoded, multiplied, passive-clamped, and ignores alpha");
  const auto before = pixels;
  require(!passiveTint({1, 2, 3}, {1, 1, 1}, pixels) && pixels == before,
          "Truncated pixels cannot replace an existing conversion");
  require(!passiveTint({1, 2, 3, 4}, {std::numeric_limits<float>::infinity(), 1, 1}, pixels),
          "Nonfinite optical tint is rejected");
  std::puts("Refract material policy passed");
}
