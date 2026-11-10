#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace proxy::sky {
inline std::vector<float> decodeRgbs(const std::vector<uint8_t> &rgba) {
  if (rgba.size() % 4) throw std::invalid_argument("Incomplete RGBS sky pixels");
  std::vector<float> linear(rgba.size(), 1);
  for (size_t pixel = 0; pixel < rgba.size(); pixel += 4)
    for (unsigned c = 0; c < 3; ++c)
      linear[pixel + c] = float(rgba[pixel + c]) * rgba[pixel + 3] * (8.f / (255 * 255));
  return linear;
}
// Static two-dimensional HDR sky faces only. Resolve the high-resolution image
// resource explicitly: VTF 7.3+ can have trailing non-image resources.
inline std::vector<float> decodeHdrVtf(const std::vector<uint8_t> &file, uint32_t &width, uint32_t &height) {
  auto fail = [] { throw std::runtime_error("Invalid or unsupported static HDR sky VTF"); };
  auto word = [&](size_t offset, unsigned bytes = 4) {
    if (offset > file.size() || bytes > file.size() - offset) fail();
    uint32_t value = 0;
    for (unsigned i = 0; i < bytes; ++i) value |= uint32_t(file[offset + i]) << (8 * i);
    return value;
  };
  if (file.size() < 64 || std::memcmp(file.data(), "VTF\0", 4) || word(4) != 7 || word(8) > 5) fail();
  const auto minor = word(8), header = word(12), w = word(16, 2), h = word(18, 2);
  const auto format = word(52), levels = word(56, 1);
  const unsigned channels = format == 28 ? 3 : 4;
  const unsigned scalarBytes = format == 24 || format == 25 ? 2 : 4;
  if ((format != 24 && format != 25 && format != 28 && format != 29) ||
      !w || !h || w > 8192 || h > 8192 || uint64_t(w) * h * 16 > (64u << 20) ||
      word(24, 2) != 1 || (word(20) & 0x4000) || !levels ||
      (minor >= 2 && word(63, 2) > 1) || header < 64 || header > file.size()) fail();
  uint64_t offset = header;
  if (minor >= 3) {
    const auto resources = word(68);
    if (resources > 32 || header < 80 + uint64_t(resources) * 8) fail();
    bool found = false;
    for (uint32_t i = 0; i < resources; ++i) {
      const size_t entry = 80 + i * 8;
      if (word(entry, 3) == 0x30) {
        if (found || (word(entry + 3, 1) & 2)) fail();
        offset = word(entry + 4);
        found = true;
      }
    }
    if (!found || offset < header) fail();
  } else {
    const auto thumbnailWidth = word(61, 1), thumbnailHeight = word(62, 1);
    if (thumbnailWidth && thumbnailHeight) {
      if (word(57) != 13) fail(); // legacy thumbnails are BC1
      offset += uint64_t((thumbnailWidth + 3) / 4) * ((thumbnailHeight + 3) / 4) * 8;
    }
  }
  unsigned maximumLevels = 0;
  for (auto edge = std::max(w, h); edge; edge >>= 1) ++maximumLevels;
  if (levels > maximumLevels) fail();
  for (unsigned mip = 1; mip < levels; ++mip)
    offset += uint64_t(std::max(1u, w >> mip)) * std::max(1u, h >> mip) * channels * scalarBytes;
  const uint64_t bytes = uint64_t(w) * h * channels * scalarBytes;
  if (offset > file.size() || bytes > file.size() - offset) fail();
  std::vector<float> pixels(size_t(w) * h * 4, 1);
  for (size_t pixel = 0; pixel < size_t(w) * h; ++pixel) {
    for (unsigned c = 0; c < 3; ++c) {
      const size_t source = size_t(offset) + (pixel * channels + c) * scalarBytes;
      float value;
      if (format == 24) {
        const auto bits = word(source, 2), exponent = (bits >> 10) & 31, fraction = bits & 1023;
        if (exponent == 31) fail();
        value = std::ldexp(float(fraction + (exponent ? 1024 : 0)), (exponent ? int(exponent) - 15 : -14) - 10);
        if (bits & 0x8000) value = -value;
      } else if (format == 25) {
        // Source's integer HDR sky encoding stores radiance divided by 16.
        value = float(word(source, 2)) * (16.f / 65535);
      } else {
        std::memcpy(&value, file.data() + source, 4);
      }
      if (!std::isfinite(value) || value < 0) fail();
      pixels[pixel * 4 + c] = value;
    }
  }
  width = w; height = h;
  return pixels;
}
} // namespace proxy::sky
