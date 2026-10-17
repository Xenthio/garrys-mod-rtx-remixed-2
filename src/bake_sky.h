#pragma once

#include <remix/remix_c.h>

namespace proxy::remix {
// The first complete sky capture is the map-load bake environment. Later live
// sky animation must not continuously invalidate the static-lighting bake.
class BakeSky {
public:
  bool capture(const remixapi_Interface &api, const remixapi_TextureInfo &texture, float scale);
  void reset(const remixapi_Interface &api, uint64_t mapId = 0);
  remixapi_LightHandle light() const { return m_light; }

private:
  uint64_t m_mapId = 0;
  remixapi_TextureHandle m_texture = nullptr;
  remixapi_LightHandle m_light = nullptr;
};
} // namespace proxy::remix
