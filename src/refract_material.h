#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace proxy::refract {

using Values = std::map<std::string, std::string>;

// Source's screen-space distortion strength has no physical IOR equivalent.
// The importer uses a finite optical sheet, independently of $refractamount.
struct Description {
  std::string normalTexture;
  std::string tintTexture;
  std::string animatedTexture;
  std::array<float, 3> tint = {1, 1, 1};
  std::array<float, 2> scroll = {};
  float framesPerSecond = 0;
  bool noCull = false;
};

// Inputs are the lowercase keys produced by the Source KeyValues reader.
// On failure, output remains unchanged and reason names the unsupported input.
bool describe(const Values &values, const Values &proxies, Description &output, std::string &reason);

// Source Refract multiplies its linear tint by twice the sRGB texture colour.
// A passive sheet cannot amplify light: clamp the result to unit transmittance.
// Alpha is deliberately ignored; Source uses this texture's RGB channels.
bool passiveTint(const std::vector<uint8_t> &rgba, const std::array<float, 3> &tint,
                 std::vector<float> &linear);

} // namespace proxy::refract
