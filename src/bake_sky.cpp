#include "bake_sky.h"
#include "bake_sky_cache.h"
#include <cstdio>
#include <cwchar>

namespace proxy::remix {
bool BakeSky::capture(const remixapi_Interface &api, const remixapi_TextureInfo &source, float scale) {
  if (m_light)
    return true;
  auto texture = source;
  const auto stable = canonicalBakeSky(source, scale, bakeSkyCachePath(m_mapId));
  if (stable.hash)
    texture.hash = stable.hash;
  if (stable.reused) {
    texture.data = stable.pixels.data();
    std::puts("Map-load bake sky: reused bounded capture-noise reference");
  }
  // Own a separate texture so replacing the live dome cannot release this one.
  texture.hash ^= 0x62616b65736b7901ull;
  if (!texture.hash)
    texture.hash = 1;
  remixapi_TextureHandle image = nullptr;
  if (api.CreateTexture(&texture, &image) != REMIXAPI_ERROR_CODE_SUCCESS)
    return false;
  wchar_t path[32];
  std::swprintf(path, 32, L"0x%llx", static_cast<unsigned long long>(texture.hash));
  remixapi_LightInfoDomeEXT dome = {};
  dome.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT;
  dome.transform = {{{1, 0, 0, 0}, {0, 0, 1, 0}, {0, 1, 0, 0}}};
  dome.colorTexture = path;
  remixapi_LightInfo light = {};
  light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  light.pNext = &dome;
  light.hash = 0x5b7d0e0000000002ull;
  light.radiance = {scale, scale, scale};
  remixapi_LightHandle handle = nullptr;
  if (api.CreateLight(&light, &handle) != REMIXAPI_ERROR_CODE_SUCCESS) {
    api.DestroyTexture(image);
    return false;
  }
  m_texture = image;
  m_light = handle;
  return true;
}

void BakeSky::reset(const remixapi_Interface &api, uint64_t mapId) {
  if (m_light)
    api.DestroyLight(m_light);
  if (m_texture)
    api.DestroyTexture(m_texture);
  m_light = nullptr;
  m_texture = nullptr;
  m_mapId = mapId;
}
} // namespace proxy::remix
