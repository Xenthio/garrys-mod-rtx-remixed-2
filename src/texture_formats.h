#pragma once
#include <cstdint>
#include <vector>

// CPU decoders for block-compressed textures (UI textures must be RGBA8).
namespace proxy::formats {

// Decodes a tightly packed BC1 (DXT1) or BC3 (DXT5) image to RGBA8
// (R in the lowest byte of each pixel).
std::vector<uint8_t> decodeBC1(const uint8_t *blocks, uint32_t width, uint32_t height);
std::vector<uint8_t> decodeBC3(const uint8_t *blocks, uint32_t width, uint32_t height);

} // namespace proxy::formats
