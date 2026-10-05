#include "texture_formats.h"

#include <cstring>

namespace proxy::formats {
namespace {

void rgb565(uint16_t c, uint8_t out[3]) {
  out[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
  out[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
  out[2] = uint8_t((c & 31) * 255 / 31);
}

// Decodes the 8-byte color part of a BC1/BC3 block into 16 RGBA pixels.
// `bc1Alpha` enables BC1's 3-color mode with transparent black.
void colorBlock(const uint8_t *block, bool bc1Alpha, uint8_t pixels[16][4]) {
  uint16_t c0, c1;
  std::memcpy(&c0, block, 2);
  std::memcpy(&c1, block + 2, 2);
  uint8_t palette[4][4];
  rgb565(c0, palette[0]);
  rgb565(c1, palette[1]);
  for (int i = 0; i < 4; ++i)
    palette[i][3] = 255;
  if (c0 > c1 || !bc1Alpha) {
    for (int k = 0; k < 3; ++k) {
      palette[2][k] = uint8_t((2 * palette[0][k] + palette[1][k]) / 3);
      palette[3][k] = uint8_t((palette[0][k] + 2 * palette[1][k]) / 3);
    }
  } else {
    for (int k = 0; k < 3; ++k) {
      palette[2][k] = uint8_t((palette[0][k] + palette[1][k]) / 2);
      palette[3][k] = 0;
    }
    palette[3][3] = 0;
  }
  uint32_t bits;
  std::memcpy(&bits, block + 4, 4);
  for (int i = 0; i < 16; ++i)
    std::memcpy(pixels[i], palette[(bits >> (2 * i)) & 3], 4);
}

template <class Decode>
std::vector<uint8_t> decode(const uint8_t *blocks, uint32_t width, uint32_t height, uint32_t blockBytes, Decode fn) {
  std::vector<uint8_t> out(size_t(width) * height * 4);
  const uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
  uint8_t pixels[16][4];
  for (uint32_t by = 0; by < bh; ++by) {
    for (uint32_t bx = 0; bx < bw; ++bx) {
      fn(blocks + (size_t(by) * bw + bx) * blockBytes, pixels);
      for (uint32_t y = 0; y < 4 && by * 4 + y < height; ++y)
        for (uint32_t x = 0; x < 4 && bx * 4 + x < width; ++x)
          std::memcpy(&out[((size_t(by) * 4 + y) * width + bx * 4 + x) * 4], pixels[y * 4 + x], 4);
    }
  }
  return out;
}

} // namespace

std::vector<uint8_t> decodeBC1(const uint8_t *blocks, uint32_t width, uint32_t height) {
  return decode(blocks, width, height, 8, [](const uint8_t *b, uint8_t p[16][4]) { colorBlock(b, true, p); });
}

std::vector<uint8_t> decodeBC3(const uint8_t *blocks, uint32_t width, uint32_t height) {
  return decode(blocks, width, height, 16, [](const uint8_t *b, uint8_t p[16][4]) {
    colorBlock(b + 8, false, p);
    uint8_t a[8] = {b[0], b[1]};
    if (a[0] > a[1]) {
      for (int i = 1; i < 7; ++i)
        a[i + 1] = uint8_t(((7 - i) * a[0] + i * a[1]) / 7);
    } else {
      for (int i = 1; i < 5; ++i)
        a[i + 1] = uint8_t(((5 - i) * a[0] + i * a[1]) / 5);
      a[6] = 0;
      a[7] = 255;
    }
    uint64_t bits = 0;
    std::memcpy(&bits, b + 2, 6);
    for (int i = 0; i < 16; ++i)
      p[i][3] = a[(bits >> (3 * i)) & 7];
  });
}

} // namespace proxy::formats
