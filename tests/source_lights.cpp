#include "source_lights.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
remixapi_LightInfoSphereEXT sphere;
remixapi_LightInfoDistantEXT distant;
remixapi_Float3D radiance;
unsigned calls = 0;
bool reject = false;
void require(bool value, const char *message) {
  if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
remixapi_ErrorCode REMIXAPI_CALL capture(const remixapi_LightInfo *info, remixapi_LightHandle *out) {
  ++calls;
  radiance = info->radiance;
  const auto type = *static_cast<const remixapi_StructType *>(info->pNext);
  if (type == REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT)
    sphere = *static_cast<const remixapi_LightInfoSphereEXT *>(info->pNext);
  else if (type == REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT)
    distant = *static_cast<const remixapi_LightInfoDistantEXT *>(info->pNext);
  else
    require(false, "Unknown native light shape");
  *out = reject ? nullptr : reinterpret_cast<remixapi_LightHandle>(uintptr_t(info->hash));
  return reject ? REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS : REMIXAPI_ERROR_CODE_SUCCESS;
}
} // namespace

int main() {
  using namespace proxy::lighting;
  constexpr float pi = 3.14159265f;
  remixapi_Interface api = {};
  api.CreateLight = capture;
  SourceLight light = {};
  light.type = 1;
  light.color[0] = 4;
  light.color[1] = 2;
  light.color[2] = 1;
  light.position[0] = 128;
  light.attenuation2 = 1;
  require(createLight(api, light, 123), "Point conversion failed");
  require(sphere.position.x == 128 && sphere.radius == 2 && !sphere.shaping_hasvalue,
          "Point positions/radii retain Source units and isotropic emission");
  require(std::abs(radiance.x * pi * sphere.radius * sphere.radius - 4) < 1e-5f,
          "Quadratic point conversion preserves forward radiant intensity");
  const auto pointRadiance = radiance;
  light.type = kSurface;
  light.direction[2] = -7;
  require(createLight(api, light, 124), "Surface conversion failed");
  require(sphere.shaping_hasvalue && sphere.shaping_value.direction.z == -1 &&
              sphere.shaping_value.coneAngleDegrees == 90 && sphere.shaping_value.coneSoftness == 0 &&
              sphere.shaping_value.focusExponent == 1,
          "Texture lights retain normal-oriented cosine emission and an unsoftened hemisphere boundary");
  require(radiance.x == pointRadiance.x && sphere.radius == 2 && sphere.position.x == 128,
          "Sidedness correction does not rescale frontal energy or relocate embedded lights");
  light.direction[2] = 0;
  const auto beforeInvalid = calls;
  require(!createLight(api, light, 125) && calls == beforeInvalid,
          "Invalid surface normal cannot silently produce an isotropic light");
  light.type = 3;
  light.direction[2] = -2;
  light.phiDot = .5f;
  light.thetaDot = std::cos(pi / 6);
  require(createLight(api, light, 126) && sphere.shaping_hasvalue &&
              sphere.shaping_value.direction.z == -1 &&
              std::abs(sphere.shaping_value.coneAngleDegrees - 60) < .001f &&
              std::abs(sphere.shaping_value.coneSoftness - .5f) < .001f &&
              sphere.shaping_value.focusExponent == 0,
          "Spot angular transition and normalization are preserved");
  light.type = 2;
  require(createLight(api, light, 127, 3, 2) && distant.direction.z == -1 &&
              std::abs(radiance.x - 8 * pi) < 1e-5f && distant.angularDiameterDegrees == .5f,
          "Distant light scale and angular diameter are preserved");
  light.color[0] = std::numeric_limits<float>::infinity();
  require(!createLight(api, light, 128), "Nonfinite radiance rejected");
  light.color[0] = 1;
  reject = true;
  require(!createLight(api, light, 129), "Runtime rejection is propagated");
  std::puts("Source light units, intensity, surface sidedness, spot cones and ABI passed");
}
