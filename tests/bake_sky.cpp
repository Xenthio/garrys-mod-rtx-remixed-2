#include "bake_sky.h"
#include <cstdio>

namespace {
unsigned textures = 0, lights = 0, destroyedTextures = 0, destroyedLights = 0;
bool refuseLight = false;
uint64_t capturedHash = 0;
remixapi_ErrorCode REMIXAPI_CALL createTexture(const remixapi_TextureInfo *info, remixapi_TextureHandle *out) {
  ++textures;
  capturedHash = info->hash;
  *out = reinterpret_cast<remixapi_TextureHandle>(uintptr_t(textures));
  return REMIXAPI_ERROR_CODE_SUCCESS;
}
remixapi_ErrorCode REMIXAPI_CALL createLight(const remixapi_LightInfo *, remixapi_LightHandle *out) {
  ++lights;
  if (refuseLight)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  *out = reinterpret_cast<remixapi_LightHandle>(uintptr_t(lights));
  return REMIXAPI_ERROR_CODE_SUCCESS;
}
remixapi_ErrorCode REMIXAPI_CALL destroyTexture(remixapi_TextureHandle) {
  ++destroyedTextures;
  return REMIXAPI_ERROR_CODE_SUCCESS;
}
remixapi_ErrorCode REMIXAPI_CALL destroyLight(remixapi_LightHandle) {
  ++destroyedLights;
  return REMIXAPI_ERROR_CODE_SUCCESS;
}
} // namespace

int main() {
  remixapi_Interface api = {};
  api.CreateTexture = &createTexture;
  api.CreateLight = &createLight;
  api.DestroyTexture = &destroyTexture;
  api.DestroyLight = &destroyLight;
  proxy::remix::BakeSky sky;
  remixapi_TextureInfo texture = {};
  texture.hash = 123;
  if (!sky.capture(api, texture, 1) || !sky.light() || capturedHash == texture.hash)
    return 1;
  const auto initial = sky.light();
  texture.hash = 456;
  if (!sky.capture(api, texture, 2) || sky.light() != initial || textures != 1 || lights != 1)
    return 1;
  sky.reset(api);
  sky.reset(api);
  if (sky.light() || destroyedLights != 1 || destroyedTextures != 1)
    return 1;
  refuseLight = true;
  if (sky.capture(api, texture, 1) || sky.light() || destroyedTextures != 2)
    return 1;
  refuseLight = false;
  if (!sky.capture(api, texture, 1) || !sky.light())
    return 1;
  sky.reset(api);
  if (destroyedLights != 2 || destroyedTextures != 3)
    return 1;
  std::puts("Map-load sky ownership, animation isolation, failure and reset passed");
  return 0;
}
