#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace proxy::bsp::lightmaps {
struct Chart {
  uint32_t firstIndex = 0, width = 0, height = 0;
  std::vector<float> texcoords; // two luxel coordinates per indexed corner
};
struct Mapping {
  float vectors[2][4];
  int32_t minimum[2], size[2];
  std::array<float, 2> project(const float point[3]) const {
    std::array<float, 2> uv;
    for (uint32_t c = 0; c < 2; ++c)
      uv[c] = vectors[c][0] * point[0] + vectors[c][1] * point[1] + vectors[c][2] * point[2] + vectors[c][3] -
              float(minimum[c]);
    return uv;
  }
};
// Input coordinates belong to the original flat face/grid. Displacement
// offsets and material UVs must never change the chart mapping.
inline std::optional<Chart> capture(const Mapping &mapping, uint32_t firstIndex,
                                    const std::vector<uint32_t> &indices, uint32_t vertexBase,
                                    const std::vector<std::array<float, 2>> &coordinates) {
  if (mapping.size[0] < 1 || mapping.size[1] < 1 || mapping.size[0] >= 4096 || mapping.size[1] >= 4096 ||
      firstIndex >= indices.size() || firstIndex % 3 || (indices.size() - firstIndex) % 3)
    return {};
  Chart chart{firstIndex, uint32_t(mapping.size[0] + 1), uint32_t(mapping.size[1] + 1), {}};
  chart.texcoords.reserve((indices.size() - firstIndex) * 2);
  for (size_t corner = firstIndex; corner < indices.size(); ++corner) {
    if (indices[corner] < vertexBase || indices[corner] - vertexBase >= coordinates.size())
      return {};
    const auto &uv = coordinates[indices[corner] - vertexBase];
    for (uint32_t c = 0; c < 2; ++c) {
      const float extent = float(mapping.size[c]);
      // BSP rounding at face edges is harmless; a materially different
      // coordinate is invalid and keeps this surface on probe lighting.
      if (!std::isfinite(uv[c]) || uv[c] < -.01f || uv[c] > extent + .01f)
        return {};
      chart.texcoords.push_back(std::clamp(uv[c], 0.f, extent));
    }
  }
  bool area = false;
  int winding = 0;
  for (size_t corner = 0; corner < chart.texcoords.size(); corner += 6) {
    const auto *uv = chart.texcoords.data() + corner;
    const double ux = double(uv[2]) - uv[0], uy = double(uv[3]) - uv[1];
    const double vx = double(uv[4]) - uv[0], vy = double(uv[5]) - uv[1];
    const double signedArea = ux * vy - uy * vx;
    if (std::abs(signedArea) <= 1e-12)
      continue;
    const int direction = signedArea > 0 ? 1 : -1;
    // A BSP face/grid is convex in its original lightmap plane. Rounding
    // can fold a nearly collinear fan triangle over its neighbour. Keep the
    // geometry, but leave this face on probes rather than publish an atlas
    // mapping that overlaps and would invalidate the complete map capture.
    if (winding && winding != direction)
      return {};
    winding = direction;
    area = true;
  }
  if (!area)
    return {}; // an entirely collinear fan has no surface lightmap area
  return chart;
}
} // namespace proxy::bsp::lightmaps
