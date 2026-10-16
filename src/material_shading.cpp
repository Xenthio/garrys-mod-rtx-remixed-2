#include "engine_materials.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace proxy::engine_materials {

Shading::Mode shaderMode(const char *shaderName) {
  std::string name = shaderName ? shaderName : "";
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  auto is = [&](const char *prefix) {
    const size_t n = std::strlen(prefix);
    // The shader or one of its hardware fallbacks ("unlitgeneric_dx8").
    return name.compare(0, n, prefix) == 0 && (name.size() == n || name[n] == '_');
  };
  if (is("unlitgeneric") || is("unlittwotexture") || is("monitorscreen") || is("sprite"))
    return Shading::Unlit;
  if (is("vertexlitgeneric") || is("lightmappedgeneric"))
    return Shading::SelfIllumBaseAlpha;
  return Shading::Lit;
}

float fresnelShare(float minimum, float maximum, float exponent) {
  // The shaders emit mask * saturate(min + (max - min) * (N.V)^exp): the mean
  // of (N.V)^exp over the projected hemisphere is 2 / (exp + 2).
  if (!std::isfinite(minimum) || !std::isfinite(maximum) || !std::isfinite(exponent) || exponent < 0)
    return 1;
  return std::clamp(minimum + (maximum - minimum) * 2 / (exponent + 2), 0.0f, 1.0f);
}

} // namespace proxy::engine_materials
