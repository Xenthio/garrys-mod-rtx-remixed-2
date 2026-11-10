#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>
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
  // Displacement lightmaps cover the normalized quad, starting at DispInfo's
  // start corner. Their dimensions need not match TexInfo's brush projection.
  std::array<float, 2> displacement(float s, float t) const {
    return {s * float(size[0]), t * float(size[1])};
  }
};
// Input coordinates belong to the original flat face/grid. Displacement
// offsets and material UVs must never change the chart mapping.
// indexEnd is exclusive; zero captures the remaining indices of an ordinary face.
inline std::optional<Chart> capture(const Mapping &mapping, uint32_t firstIndex,
                                    const std::vector<uint32_t> &indices, uint32_t vertexBase,
                                    const std::vector<std::array<float, 2>> &coordinates,
                                    size_t indexEnd = 0) {
  if (!indexEnd)
    indexEnd = indices.size();
  if (indexEnd > indices.size() || indexEnd <= firstIndex || indexEnd % 3)
    return {};
  if (mapping.size[0] < 1 || mapping.size[1] < 1 || mapping.size[0] >= 4096 || mapping.size[1] >= 4096 ||
      firstIndex >= indices.size() || firstIndex % 3 || (indexEnd - firstIndex) % 3)
    return {};
  Chart chart{firstIndex, uint32_t(mapping.size[0] + 1), uint32_t(mapping.size[1] + 1), {}};
  chart.texcoords.reserve((indexEnd - firstIndex) * 2);
  for (size_t corner = firstIndex; corner < indexEnd; ++corner) {
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
// Rounded collinear BSP fan corners can reverse a microscopic triangle. Keep
// that geometry on probes, but preserve charted contiguous ranges around it.
// Native validation still checks every published range for positive overlap.
inline std::vector<Chart> captureFan(const Mapping &mapping, uint32_t firstIndex,
                                     const std::vector<uint32_t> &indices, uint32_t vertexBase,
                                     const std::vector<std::array<float, 2>> &coordinates) {
  if (auto full = capture(mapping, firstIndex, indices, vertexBase, coordinates)) {
    std::vector<Chart> result;
    result.push_back(std::move(*full));
    return result;
  }
  if (firstIndex >= indices.size() || firstIndex % 3 || (indices.size() - firstIndex) % 3 ||
      mapping.size[0] < 1 || mapping.size[1] < 1 || mapping.size[0] >= 4096 || mapping.size[1] >= 4096)
    return {};
  std::vector<std::array<float, 2>> corners;
  for (size_t i = firstIndex; i < indices.size(); ++i) {
    if (indices[i] < vertexBase || indices[i] - vertexBase >= coordinates.size() ||
        ((i - firstIndex) % 3 == 0 && indices[i] != indices[firstIndex]))
      return {};
    auto uv = coordinates[indices[i] - vertexBase];
    for (uint32_t axis = 0; axis < 2; ++axis) {
      const float extent = float(mapping.size[axis]);
      if (!std::isfinite(uv[axis]) || uv[axis] < -.01f || uv[axis] > extent + .01f)
        return {};
      uv[axis] = std::clamp(uv[axis], 0.f, extent);
    }
    corners.push_back(uv);
  }
  const auto area = [&](size_t i) {
    const auto a = corners[i], b = corners[i + 1], c = corners[i + 2];
    return (double(b[0]) - a[0]) * (double(c[1]) - a[1]) - (double(b[1]) - a[1]) * (double(c[0]) - a[0]);
  };
  double dominant = 0;
  for (size_t i = 0; i < corners.size(); i += 3)
    if (std::abs(area(i)) > std::abs(dominant))
      dominant = area(i);
  if (std::abs(dominant) <= 1e-12)
    return {};
  std::vector<size_t> excluded;
  for (size_t i = 0; i < corners.size(); i += 3) {
    const double signedArea = area(i);
    double longest = 0;
    for (size_t edge = 0; edge < 3; ++edge) {
      const auto a = corners[i + edge], b = corners[i + (edge + 1) % 3];
      longest = std::max(longest, std::hypot(double(b[0]) - a[0], double(b[1]) - a[1]));
    }
    // Perpendicular UV altitude, independent of triangle length/atlas density.
    // Larger folds still refuse the complete face instead of hiding bad data.
    if (std::abs(signedArea) <= longest * 1e-4) {
      excluded.push_back(size_t(firstIndex) + i);
      continue;
    }
    if (signedArea * dominant < 0)
      return {};
  }
  if (excluded.empty())
    return {};
  std::vector<Chart> result;
  size_t first = firstIndex;
  excluded.push_back(indices.size());
  for (const auto end : excluded) {
    if (first < end)
      if (auto chart = capture(mapping, uint32_t(first), indices, vertexBase, coordinates, end))
        result.push_back(std::move(*chart));
    first = end + 3;
  }
  return result;
}
} // namespace proxy::bsp::lightmaps
