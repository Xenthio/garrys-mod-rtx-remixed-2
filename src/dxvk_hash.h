#pragma once
#include "remix_output.h"
#include <cstddef>
#include <cstdint>

// Replacement keys as dxvk-remix computes them, so USD mods captured with the
// compatibility mod's dxvk-remix path (mat_<hash>, mesh_<hash>) apply here.
namespace proxy::dxvk {

// Texture hash: XXH3_64bits of mip 0 as the game wrote it, rows packed at a
// 4-byte aligned pitch (d3d9_common_texture.cpp SetupForRtxFrom).
uint64_t textureHash(const uint8_t *packedMip0, size_t size);

// Geometry asset hash for rtx.geometryAssetHashRuleString =
// positions,indices,geometrydescriptor (d3d9_rtx_geometry.cpp, rtx_hashing):
//   XXH64(&descriptor, 8, XXH64(&indices, 8, positions)).
// The mesh replacement key is this XOR the base texture's hash. 0 if the draw
// cannot be hashed (non-indexed, degenerate, unknown data).
uint64_t geometryHash(const remix::Draw &d);

} // namespace proxy::dxvk
