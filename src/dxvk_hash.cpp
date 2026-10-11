#include "dxvk_hash.h"

#include <algorithm>
#include <cstring>
#include <vector>

#define XXH_INLINE_ALL
#include "xxhash.h"

namespace proxy::dxvk {

uint64_t textureHash(const uint8_t *packedMip0, size_t size) { return XXH3_64bits(packedMip0, size); }
uint64_t legacyTextureHash(const uint8_t *packedMip0, size_t size) { return XXH64(packedMip0, size, 0); }

uint64_t geometryHash(const remix::Draw &d) {
  if (!d.indices.data || !d.elements)
    return 0;
  uint32_t indexCount, topology; // VkPrimitiveTopology
  switch (d.type) {
  case D3DPT_TRIANGLELIST:
    indexCount = d.primCount * 3;
    topology = 3;
    break;
  case D3DPT_TRIANGLESTRIP:
    indexCount = d.primCount + 2;
    topology = 4;
    break;
  case D3DPT_TRIANGLEFAN:
    indexCount = d.primCount + 2;
    topology = 5;
    break;
  default:
    return 0;
  }
  const uint32_t indexSize = d.indices.is32 ? 4 : 2;
  const uint64_t first = uint64_t(d.startIndex) * indexSize, bytes = uint64_t(indexCount) * indexSize;
  if (!indexCount || first + bytes > d.indices.size)
    return 0;

  // Indices are rebased onto the smallest one and hashed at their own width.
  std::vector<uint32_t> values(indexCount);
  for (uint32_t i = 0; i < indexCount; ++i) {
    uint32_t v = 0;
    std::memcpy(&v, d.indices.data + first + size_t(i) * indexSize, indexSize);
    values[i] = v;
  }
  const auto [lo, hi] = std::minmax_element(values.begin(), values.end());
  const uint32_t low = *lo, high = *hi;
  if (low == high)
    return 0; // dxvk-remix skips these draws
  std::vector<uint8_t> rebased(bytes);
  for (uint32_t i = 0; i < indexCount; ++i) {
    const uint32_t v = values[i] - low;
    std::memcpy(rebased.data() + size_t(i) * indexSize, &v, indexSize);
  }
  const uint64_t indexHash = XXH3_64bits(rebased.data(), rebased.size());

  // Positions: the raw position element of each distinct referenced vertex,
  // in ascending index order, chained through the seed.
  const D3DVERTEXELEMENT9 *pos = nullptr;
  for (auto *e = d.elements; e->Stream != 0xff; ++e)
    if ((e->Usage == D3DDECLUSAGE_POSITION || e->Usage == D3DDECLUSAGE_POSITIONT) && e->UsageIndex == 0) {
      pos = e;
      break;
    }
  if (!pos || pos->Stream >= 4 || (pos->Type != D3DDECLTYPE_FLOAT3 && pos->Type != D3DDECLTYPE_FLOAT4))
    return 0;
  const uint32_t elementSize = pos->Type == D3DDECLTYPE_FLOAT3 ? 12 : 16;
  const auto &s = d.streams[pos->Stream];
  if (!s.data)
    return 0;
  // Distinct indices in ascending order, by a bitmap over [low, high].
  std::vector<uint64_t> seen((size_t(high - low) >> 6) + 1, 0);
  for (const uint32_t v : values)
    seen[(v - low) >> 6] |= 1ull << ((v - low) & 63);
  uint64_t positionHash = 0;
  for (size_t word = 0; word < seen.size(); ++word)
    for (uint64_t bits = seen[word]; bits; bits &= bits - 1) {
      const uint32_t index = low + uint32_t(word * 64 + __builtin_ctzll(bits));
      const int64_t at = int64_t(s.offset) + (int64_t(d.baseVertex) + index) * s.stride + pos->Offset;
      if (at < 0 || uint64_t(at) + elementSize > s.size)
        return 0;
      positionHash = XXH3_64bits_withSeed(s.data + at, elementSize, positionHash);
    }

  const uint32_t descriptor[4] = {indexCount, high - low + 1, topology, d.indices.is32 ? 1u : 0u};
  uint64_t descriptorHash = 0;
  for (const uint32_t field : descriptor)
    descriptorHash = XXH3_64bits_withSeed(&field, 4, descriptorHash);

  uint64_t asset = positionHash;
  asset = XXH64(&indexHash, 8, asset);
  return XXH64(&descriptorHash, 8, asset);
}

} // namespace proxy::dxvk
