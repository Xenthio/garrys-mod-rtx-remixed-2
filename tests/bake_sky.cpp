#include "bake_sky.h"
#include "bake_sky_cache.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <algorithm>

namespace {
unsigned textures = 0, lights = 0, destroyedTextures = 0, destroyedLights = 0;
bool refuseLight = false;
uint64_t capturedHash = 0;
uint8_t capturedPixel = 0;
remixapi_ErrorCode REMIXAPI_CALL createTexture(const remixapi_TextureInfo *info, remixapi_TextureHandle *out) {
  ++textures;
  capturedHash = info->hash;
  if (info->data && info->dataSize) capturedPixel = *static_cast<const uint8_t *>(info->data);
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
  const auto reference = std::filesystem::temp_directory_path() /
      ("openremix-sky-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) + ".sky");
  std::vector<uint8_t> pixels(16 * 16 * 4, 100);
  auto source = texture;
  source.width = source.height = 16;
  source.depth = source.mipLevels = 1;
  source.format = REMIXAPI_FORMAT_R8G8B8A8_SRGB;
  source.data = pixels.data();
  source.dataSize = pixels.size();
  const auto stored = proxy::remix::canonicalBakeSky(source, 1, reference);
  std::fill(pixels.begin(), pixels.end(), 20);
  _putenv_s("OPENREMIX_PROXY_BAKE_SKY_REFERENCE", reference.string().c_str());
  if (!sky.capture(api, source, 1) || !sky.light() || capturedPixel != 100 ||
      capturedHash != (stored.hash ^ 0x62616b65736b7901ull))
    return 1;
  sky.reset(api);
  std::filesystem::remove(reference);
  _putenv_s("OPENREMIX_PROXY_BAKE_SKY_REFERENCE", "missing-diagnostic-reference.sky");
  const auto previousTextures = textures;
  if (sky.capture(api, texture, 1) || sky.light() || textures != previousTextures)
    return 1;
  _putenv_s("OPENREMIX_PROXY_BAKE_SKY_REFERENCE", "");
  if (!sky.capture(api, texture, 1) || !sky.light())
    return 1;
  sky.reset(api);
  std::puts("Map-load sky ownership, animation isolation, failure and reset passed");
  return 0;
}
