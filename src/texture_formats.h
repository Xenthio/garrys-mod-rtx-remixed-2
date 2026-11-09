#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// CPU decoders for block-compressed textures (UI textures must be RGBA8).
namespace proxy::formats {

// Decodes a tightly packed BC1 (DXT1), BC2 (DXT3) or BC3 (DXT5) image to
// RGBA8 (R in the lowest byte of each pixel).
std::vector<uint8_t> decodeBC1(const uint8_t *blocks, uint32_t width, uint32_t height);
std::vector<uint8_t> decodeBC2(const uint8_t *blocks, uint32_t width, uint32_t height);
std::vector<uint8_t> decodeBC3(const uint8_t *blocks, uint32_t width, uint32_t height);

// Rewrites BC2 (DXT3) blocks in place as BC3 (DXT5), which the runtime takes:
// the color halves are the same, the explicit 4-bit alpha becomes each
// block's interpolated alpha between its smallest and largest value.
void transcodeBC2toBC3(uint8_t *blocks, size_t bytes);

} // namespace proxy::formats
