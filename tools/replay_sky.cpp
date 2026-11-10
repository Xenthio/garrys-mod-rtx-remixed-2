#include "replay_sky.h"
#include "bake_sky_cache.h"
#include "sky_faces.h"
#include <remix/remix_native_ext.h>
#include <type_traits>
#define XXH_INLINE_ALL
#include "xxhash.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <stdexcept>

namespace proxy::replay {
namespace {
float linear(float value) {
  return value <= .04045f ? value / 12.92f : std::pow((value + .055f) / 1.055f, 2.4f);
}
uint8_t srgb(float value) {
  value = value <= .0031308f ? 12.92f * value : 1.055f * std::pow(value, 1.f/2.4f) - .055f;
  return uint8_t(std::clamp(std::lround(value * 255), 0l, 255l));
}
void check(remixapi_ErrorCode code, const char *operation) {
  if (code != REMIXAPI_ERROR_CODE_SUCCESS) throw std::runtime_error(operation);
}
}
template<typename T> std::vector<T> makeSkyDome(const std::array<bsp::SkyboxFace, 6> &faces,
                             uint32_t width, uint32_t height) {
  if (!width || !height || width > 8192 || height > 4096)
    throw std::invalid_argument("Invalid replay sky dome dimensions");
  for (const auto &face : faces)
    if (!face.width || !face.height || face.width > 8192 || face.height > 8192 ||
        (face.linearRgba.empty() ? face.rgba.size() : face.linearRgba.size()) !=
            uint64_t(face.width)*face.height*4)
      throw std::invalid_argument("Incomplete replay sky faces");
  for (const auto &face : faces)
    for (float value : face.linearRgba)
      if (!std::isfinite(value) || value < 0)
        throw std::invalid_argument("Invalid linear sky radiance");
  std::array<float, 256> decode;
  for (unsigned i = 0; i < decode.size(); ++i) decode[i] = linear(float(i)/255);
  // Texture up differs from the live camera capture basis at the upper face.
  constexpr float right[6][3]{{0,-1,0},{0,1,0},{1,0,0},{-1,0,0},{0,-1,0},{0,-1,0}};
  constexpr float up[6][3]{{0,0,1},{0,0,1},{0,0,1},{0,0,1},{-1,0,0},{1,0,0}};
  std::vector<T> output(size_t(width)*height*4);
  constexpr float pi = 3.14159265358979323846f;
  for (uint32_t y = 0; y < height; ++y) {
    const float theta = (float(y)+.5f)/height*pi;
    for (uint32_t x = 0; x < width; ++x) {
      const float phi = ((float(x)+.5f)/width-.5f)*2*pi;
      const float direction[]{std::sin(theta)*std::cos(phi), std::sin(theta)*std::sin(phi), std::cos(theta)};
      unsigned axis = 0;
      for (unsigned c = 1; c < 3; ++c)
        if (std::abs(direction[c]) > std::abs(direction[axis])) axis = c;
      const unsigned index = 2*axis + (direction[axis]<0);
      const auto &face = faces[index];
      float u = 0, v = 0;
      for (unsigned c = 0; c < 3; ++c) {
        u += direction[c]*right[index][c];
        v += direction[c]*up[index][c];
      }
      const float major = std::abs(direction[axis]);
      const float px = std::clamp((.5f+.5f*u/major)*face.width-.5f, 0.f, float(face.width-1));
      const float py = std::clamp((.5f-.5f*v/major)*face.height-.5f, 0.f, float(face.height-1));
      const uint32_t x0 = uint32_t(px), y0 = uint32_t(py);
      const uint32_t x1 = std::min(x0+1,face.width-1), y1 = std::min(y0+1,face.height-1);
      const float fx = px-x0, fy = py-y0;
      auto *pixel = output.data()+(size_t(y)*width+x)*4;
      for (unsigned c = 0; c < 3; ++c) {
        auto value = [&](uint32_t tx, uint32_t ty) {
          const size_t index = (size_t(ty)*face.width+tx)*4+c;
          return face.linearRgba.empty() ? decode[face.rgba[index]] : face.linearRgba[index];
        };
        const float filtered = (value(x0,y0)*(1-fx)+value(x1,y0)*fx)*(1-fy) +
                               (value(x0,y1)*(1-fx)+value(x1,y1)*fx)*fy;
        if constexpr (std::is_same_v<T, float>) pixel[c] = filtered;
        else pixel[c] = srgb(filtered);
      }
      pixel[3] = std::is_same_v<T, float> ? 1 : 255;
    }
  }
  return output;
}
std::vector<uint8_t> skyDome(const std::array<bsp::SkyboxFace, 6> &faces, uint32_t width, uint32_t height) {
  return makeSkyDome<uint8_t>(faces, width, height);
}
std::vector<float> skyDomeHdr(const std::array<bsp::SkyboxFace, 6> &faces, uint32_t width, uint32_t height) {
  return makeSkyDome<float>(faces, width, height);
}
Sky::~Sky() {
  if (m_light) m_api.DestroyLight(m_light);
  if (m_texture) m_api.DestroyTexture(m_texture);
}
void Sky::load(const bsp::SkyboxImages &source, const std::string &map) {
  constexpr uint32_t width = sky::kDomeWidth, height = sky::kDomeHeight;
  float scale = 1;
  if (const char *value = std::getenv("OPENREMIX_PROXY_SKY_DOME_SCALE"); value && *value) {
    char *end = nullptr;
    scale = std::strtof(value, &end);
    if (!end || *end || !std::isfinite(scale) || scale < 0)
      throw std::invalid_argument("Invalid replay sky dome scale");
  }
  remixapi_TextureInfo info{};
  info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
  info.width = width; info.height = height; info.depth = info.mipLevels = 1;
  info.format = REMIXAPI_FORMAT_R8G8B8A8_SRGB;
  std::vector<uint8_t> pixels;
  std::vector<float> hdrPixels;
  const char *reference = std::getenv("OPENREMIX_PROXY_BAKE_SKY_REFERENCE");
  if (reference && *reference) {
    pixels.resize(size_t(width)*height*4);
    info.data = pixels.data(); info.dataSize = pixels.size();
    auto captured = remix::canonicalBakeSky(info, scale, std::filesystem::u8path(reference), remix::BakeSkyReuse::ExactReference);
    if (!captured.reused) throw std::runtime_error("Replay sky reference failed shape/scale/checksum validation");
    pixels = std::move(captured.pixels);
    std::printf("Replay sky: %s, frozen live reference %s\n", map.c_str(), reference);
  } else {
    if (source.name.empty() || !source.error.empty()) {
      std::fprintf(stderr, "Replay sky unavailable: %s (%s); sky lighting differs from the live game\n",
                   source.name.c_str(), source.error.c_str());
      return;
    }
    const bool hdr = std::any_of(source.faces.begin(), source.faces.end(), [](const auto &face) {
      return !face.linearRgba.empty();
    });
    if (hdr) {
      hdrPixels = skyDomeHdr(source.faces, width, height);
      info.format = REMIXAPI_FORMAT_R32G32B32A32_SFLOAT_EXT;
    } else pixels = skyDome(source.faces, width, height);
    std::printf("Replay sky: %s, six static %s faces; no live sky animation/material proxies\n",
                 source.name.c_str(), hdr ? "linear HDR" : "LDR");
  }
  info.data = hdrPixels.empty() ? static_cast<const void *>(pixels.data()) : hdrPixels.data();
  info.dataSize = hdrPixels.empty() ? pixels.size() : hdrPixels.size() * sizeof(float);
  info.hash = hdrPixels.empty() ? XXH3_64bits(info.data, info.dataSize) | 1
      : XXH3_64bits_withSeed(info.data, info.dataSize, uint32_t(info.format)) | 1;
  std::printf("Replay sky dome: %ux%u %s, scale %.6g, content %016llx\n", width, height,
               hdrPixels.empty() ? "sRGB" : "linear float", scale,
               static_cast<unsigned long long>(info.hash));
  check(m_api.CreateTexture(&info, &m_texture), "Replay sky texture creation failed");
  wchar_t path[32]; std::swprintf(path, 32, L"0x%llx", static_cast<unsigned long long>(info.hash));
  remixapi_LightInfoDomeEXT dome{};
  dome.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT;
  dome.transform = {{{1,0,0,0},{0,0,1,0},{0,1,0,0}}};
  dome.colorTexture = path;
  remixapi_LightInfo light{};
  light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO; light.pNext = &dome;
  light.hash = 0x7265706c736b7901ull; light.radiance = {scale,scale,scale};
  check(m_api.CreateLight(&light, &m_light), "Replay sky dome creation failed");
}
} // namespace proxy::replay
