#pragma once
#include "bsp_world.h"
#include <vector>

namespace proxy::replay {
// Convert Source's six static LDR sky faces to the live dome's Z-up world
// orientation. Output is sRGB; interpolation happens in linear light.
std::vector<uint8_t> skyDome(const std::array<bsp::SkyboxFace, 6> &faces,
                             uint32_t width, uint32_t height);
// Preserves values above one. Mixed LDR/HDR faces are filtered in linear light.
std::vector<float> skyDomeHdr(const std::array<bsp::SkyboxFace, 6> &faces,
                             uint32_t width, uint32_t height);

class Sky {
public:
  explicit Sky(const remixapi_Interface &api) : m_api(api) {}
  ~Sky();
  void load(const bsp::SkyboxImages &source, const std::string &map);
  remixapi_LightHandle light() const { return m_light; }
private:
  const remixapi_Interface &m_api;
  remixapi_TextureHandle m_texture = nullptr;
  remixapi_LightHandle m_light = nullptr;
};
} // namespace proxy::replay
