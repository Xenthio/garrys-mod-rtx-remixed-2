#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// LZMA as Source stores it (deps/lzma: the LZMA SDK decoder). Newer maps
// (TF2's among them) compress their lumps, their static prop game lump and
// the files of their pakfile.
namespace proxy::lzma {

// A Source LZMA blob: "LZMA", the decoded size, the stream's size and the
// five property bytes (lzma_header_t), then the stream. False when `data` is
// not one, or does not decode to its stated size.
bool decodeSource(const uint8_t *data, size_t size, std::vector<uint8_t> &out);

// The payload of a zip entry of method 14 (LZMA): version (2 bytes), the
// properties' size (2), the properties, then the stream, decoding to
// `expected` bytes.
bool decodeZip(const uint8_t *data, size_t size, size_t expected, std::vector<uint8_t> &out);

} // namespace proxy::lzma
