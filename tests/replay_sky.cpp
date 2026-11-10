#include "replay_sky.h"
#include "sky_texture.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
int main() try {
  std::array<proxy::bsp::SkyboxFace,6> faces;
  for (unsigned i = 0; i < faces.size(); ++i) {
    faces[i].width = faces[i].height = 1;
    faces[i].rgba = {uint8_t(10+40*i),0,0,255};
  }
  const auto dome = proxy::replay::skyDome(faces,8,4);
  auto red = [&](unsigned x, unsigned y) { return dome[(y*8+x)*4]; };
  require(red(4,1)==10 && red(0,1)==50 && red(6,1)==90 && red(2,1)==130,
          "Dome horizon preserves Source world-axis face selection");
  require(red(4,0)==170 && red(4,3)==210, "Dome poles preserve world up and down");
  for (size_t i=3; i<dome.size(); i+=4) require(dome[i]==255,"Dome alpha is opaque");
  faces[4] = {1,2,{0,0,0,255,255,0,0,255}};
  const auto gradient = proxy::replay::skyDome(faces,8,4);
  require(gradient[(0*8+4)*4] > gradient[(0*8+0)*4],
          "Upper Source texture face retains its distinct vertical orientation");
  const auto rgbs = proxy::sky::decodeRgbs({255, 128, 0, 128});
  require(std::abs(rgbs[0] - 8.f*128/255) < 1e-6f && rgbs[2] == 0 && rgbs[3] == 1,
          "RGBS decodes alpha scaling before filtering without gamma conversion");
  faces[0].linearRgba = {8, .25f, 2, 1};
  const auto hdrDome = proxy::replay::skyDomeHdr(faces, 8, 4);
  require(hdrDome[(1*8+4)*4] == 8 && hdrDome[(1*8+4)*4+1] == .25f,
          "Linear dome retains HDR values and mixes LDR faces without clipping");
  require(std::abs(hdrDome[(1*8+0)*4] - std::pow((50.f/255+.055f)/1.055f, 2.4f)) < 1e-6f,
          "LDR faces are decoded exactly once in an HDR dome");
  faces[0].linearRgba.pop_back();
  bool refusedHdr = false;
  try { proxy::replay::skyDomeHdr(faces,8,4); } catch (const std::invalid_argument &) { refusedHdr = true; }
  require(refusedHdr,"Complete LDR storage must not mask incomplete HDR storage");
  faces[0].linearRgba.clear();
  std::vector<uint8_t> vtf(88 + 8);
  auto word = [&](size_t offset, uint32_t value, unsigned bytes = 4) {
    for (unsigned i = 0; i < bytes; ++i) vtf.at(offset+i) = uint8_t(value >> (8*i));
  };
  std::memcpy(vtf.data(), "VTF\0", 4);
  word(4,7); word(8,3); word(12,88); word(16,1,2); word(18,1,2);
  word(24,1,2); word(52,24); word(56,1,1); word(63,1,2);
  word(68,1); word(80,0x30); word(84,88);
  word(88,0x4800,2); word(90,0x3400,2); word(92,0x4000,2); word(94,0x3c00,2);
  uint32_t width = 0, height = 0;
  auto decoded = proxy::sky::decodeHdrVtf(vtf, width, height);
  require(width == 1 && height == 1 && decoded == std::vector<float>({8,.25f,2,1}),
          "Half-float HDR sky decodes without sRGB or integer-HDR scaling");
  vtf.resize(128,0xff);
  require(proxy::sky::decodeHdrVtf(vtf,width,height) == decoded,
          "High-resolution resource offset ignores trailing VTF resources");
  word(52,25); word(88,65535,2); word(90,0,2); word(92,32768,2);
  decoded = proxy::sky::decodeHdrVtf(vtf,width,height);
  require(decoded[0] == 16 && decoded[1] == 0 && decoded[2] > 8,
          "Integer HDR sky retains Source's sixteen-times radiance scale");
  auto refuseVtf = [&] {
    bool refused = false;
    try { proxy::sky::decodeHdrVtf(vtf,width,height); } catch (const std::runtime_error &) { refused = true; }
    require(refused,"Invalid HDR sky must not publish a partial environment");
  };
  word(24,2,2); refuseVtf(); word(24,1,2);
  word(24,0,2); refuseVtf(); word(24,1,2);
  word(20,0x4000); refuseVtf(); word(20,0);
  word(84,121); refuseVtf(); word(84,88);
  word(52,24); word(88,0x7c00,2); refuseVtf();
  word(88,0x4800,2); vtf.resize(95); refuseVtf();
  vtf.resize(104); word(52,28);
  const float rgb[] = {12, .5f, .125f};
  std::memcpy(vtf.data()+88,rgb,sizeof(rgb));
  require(proxy::sky::decodeHdrVtf(vtf,width,height) == std::vector<float>({12,.5f,.125f,1}),
          "RGB float sky gains opaque alpha without clipping radiance");
  const float invalid = std::numeric_limits<float>::quiet_NaN();
  std::memcpy(vtf.data()+88,&invalid,sizeof(invalid)); refuseVtf();
  faces[0].rgba.pop_back();
  bool refused=false;
  try { proxy::replay::skyDome(faces,8,4); } catch (const std::invalid_argument &) { refused=true; }
  require(refused,"Incomplete faces cannot publish a partial lighting environment");
  std::puts("Replay sky face selection, poles, orientation, alpha and validation passed");
} catch (const std::exception &error) {
  std::fprintf(stderr,"%s\n",error.what()); return 1;
}
