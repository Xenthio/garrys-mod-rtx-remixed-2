#include "source_lights.h"
#include <algorithm>
#include <cmath>

namespace proxy::lighting {
remixapi_LightHandle createLight(remixapi_Interface &api, const SourceLight &light, uint64_t hash,
                                float lightScale, float sunScale) {
  constexpr float pi = 3.14159265f, radius = 2, reference = 100;
  if (light.type < 1 || light.type > kSurface)
    return nullptr;
  remixapi_LightInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  info.hash = hash;
  remixapi_LightInfoSphereEXT sphere = {};
  sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
  sphere.position = {light.position[0], light.position[1], light.position[2]};
  sphere.radius = radius;
  remixapi_LightInfoDistantEXT distant = {};
  distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
  distant.angularDiameterDegrees = .5f;
  const float length = std::hypot(light.direction[0], light.direction[1], light.direction[2]);
  const bool validDirection = std::isfinite(length) && length > 1e-6f;
  remixapi_Float3D direction = {};
  if (validDirection)
    direction = {light.direction[0] / length, light.direction[1] / length, light.direction[2] / length};
  float scale;
  if (light.type == 2) {
    if (!validDirection)
      return nullptr;
    scale = sunScale * pi;
    distant.direction = direction;
    info.pNext = &distant;
  } else {
    const float falloff = light.attenuation0 + light.attenuation1 * reference +
                          light.attenuation2 * reference * reference;
    if (!(falloff > 0) || !std::isfinite(falloff))
      return nullptr;
    // Preserve the existing radiant intensity at the reference distance.
    scale = lightScale * reference * reference / (falloff * pi * radius * radius);
    if (light.type == kSurface) {
      if (!validDirection)
        return nullptr;
      // VRAD's emit_surface uses max(dot(normal, outgoing), 0), not the
      // isotropic point approximation supplied to Source's model lighting.
      // See ValveSoftware/source-sdk-2013, src/utils/vrad/lightmap.cpp,
      // GatherSampleStandardLightSSE (emit_surface).
      // Existing spherical sampling retains soft shadows and one light ID;
      // shaping restores the authored angular distribution in every backend.
      sphere.shaping_hasvalue = 1;
      sphere.shaping_value.direction = direction;
      sphere.shaping_value.coneAngleDegrees = 90;
      sphere.shaping_value.focusExponent = 1;
    } else if (light.type == 3 && validDirection && light.phiDot > -1 && light.phiDot < 1) {
      sphere.shaping_hasvalue = 1;
      sphere.shaping_value.direction = direction;
      const float outer = std::acos(light.phiDot), inner = std::acos(std::clamp(light.thetaDot, -1.f, 1.f));
      sphere.shaping_value.coneAngleDegrees = outer * 180 / pi;
      sphere.shaping_value.coneSoftness = outer > 0 ? std::clamp((outer - inner) / outer, 0.f, 1.f) : 0;
    }
    info.pNext = &sphere;
  }
  info.radiance = {light.color[0] * scale, light.color[1] * scale, light.color[2] * scale};
  for (float value : {info.radiance.x, info.radiance.y, info.radiance.z})
    if (!std::isfinite(value) || value < 0)
      return nullptr;
  remixapi_LightHandle handle = nullptr;
  return api.CreateLight(&info, &handle) == REMIXAPI_ERROR_CODE_SUCCESS ? handle : nullptr;
}
} // namespace proxy::lighting
