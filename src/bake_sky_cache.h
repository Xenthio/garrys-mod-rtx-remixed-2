#pragma once
#include <remix/remix_c.h>
#include <filesystem>
#include <vector>

namespace proxy::remix {
struct BakeSkyImage {
  std::vector<uint8_t> pixels; // Present only when reusing a previous capture.
  uint64_t hash = 0;
  bool reused = false;
};
std::filesystem::path bakeSkyCachePath(uint64_t mapId) noexcept;
// Reuse only an essentially identical RGBA8 capture. The returned pixels, not
// just their hash, must be used for baking. Larger changes replace the record.
// Invalid/unwritable caches fall back to the supplied image.
BakeSkyImage canonicalBakeSky(const remixapi_TextureInfo &source, float scale,
                              const std::filesystem::path &file) noexcept;
} // namespace proxy::remix
