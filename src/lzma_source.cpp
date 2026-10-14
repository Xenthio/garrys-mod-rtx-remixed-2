#include "lzma_source.h"

#include <cstdlib>
#include <cstring>

extern "C" {
#include "../deps/lzma/LzmaDec.h"
}

namespace proxy::lzma {
namespace {

void *allocate(ISzAllocPtr, size_t size) { return std::malloc(size); }
void release(ISzAllocPtr, void *address) { std::free(address); }
const ISzAlloc g_alloc = {allocate, release};

// Decodes `size` bytes of stream with `properties` to exactly `expected` bytes.
bool decode(const uint8_t *properties, const uint8_t *stream, size_t size, size_t expected,
            std::vector<uint8_t> &out) {
  if (expected > (1u << 30))
    return false;
  out.resize(expected);
  SizeT destination = expected, source = size;
  ELzmaStatus status;
  const SRes result = LzmaDecode(out.data(), &destination, stream, &source, properties, LZMA_PROPS_SIZE,
                                 LZMA_FINISH_ANY, &status, &g_alloc);
  return result == SZ_OK && destination == expected;
}

} // namespace

bool decodeSource(const uint8_t *data, size_t size, std::vector<uint8_t> &out) {
  constexpr size_t kHeader = 4 + 4 + 4 + LZMA_PROPS_SIZE;
  if (size < kHeader || std::memcmp(data, "LZMA", 4) != 0)
    return false;
  uint32_t actual, compressed;
  std::memcpy(&actual, data + 4, 4);
  std::memcpy(&compressed, data + 8, 4);
  if (compressed > size - kHeader)
    return false;
  return decode(data + 12, data + kHeader, compressed, actual, out);
}

bool decodeZip(const uint8_t *data, size_t size, size_t expected, std::vector<uint8_t> &out) {
  if (size < 4)
    return false;
  uint16_t propertiesSize;
  std::memcpy(&propertiesSize, data + 2, 2);
  if (propertiesSize != LZMA_PROPS_SIZE || size < 4 + size_t(propertiesSize))
    return false;
  return decode(data + 4, data + 4 + propertiesSize, size - 4 - propertiesSize, expected, out);
}

} // namespace proxy::lzma
