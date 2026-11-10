#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <remix/remix_c.h>
#include <stdexcept>
#include <utility>
#include <vector>

namespace proxy::sky {
using Vertex = remixapi_HardcodedVertex;
using Polygon = std::vector<Vertex>;
struct ClippedMesh {
  std::vector<Vertex> vertices;
  std::vector<uint32_t> indices;
  bool changed = false;
};

// Keep stored meshes and replacement identities in model space. Only clipping
// uses world coordinates; attributes at new intersections return to model space.
class Placement {
public:
  explicit Placement(const float (&matrix)[3][4]) {
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 4; ++c) {
        if (!std::isfinite(matrix[r][c]))
          throw std::invalid_argument("Non-finite sky placement");
        m_matrix[r][c] = matrix[r][c];
      }
    const std::array<float, 3> a{matrix[0][0], matrix[1][0], matrix[2][0]},
        b{matrix[0][1], matrix[1][1], matrix[2][1]}, c{matrix[0][2], matrix[1][2], matrix[2][2]};
    const auto bc = cross(b, c), ca = cross(c, a), ab = cross(a, b);
    const float det = a[0] * bc[0] + a[1] * bc[1] + a[2] * bc[2];
    if (!std::isfinite(det) || std::abs(det) < 1e-20f)
      throw std::invalid_argument("Singular sky placement");
    for (int i = 0; i < 3; ++i) {
      m_inverse[0][i] = bc[i] / det;
      m_inverse[1][i] = ca[i] / det;
      m_inverse[2][i] = ab[i] / det;
    }
  }
  Vertex world(Vertex vertex) const {
    const auto input = vertex;
    for (int r = 0; r < 3; ++r) {
      vertex.position[r] = m_matrix[r][3];
      vertex.normal[r] = 0;
      for (int c = 0; c < 3; ++c) {
        vertex.position[r] += m_matrix[r][c] * input.position[c];
        vertex.normal[r] += m_inverse[c][r] * input.normal[c];
      }
    }
    normalize(vertex.normal);
    return vertex;
  }
  Vertex local(Vertex vertex) const {
    const auto input = vertex;
    for (int r = 0; r < 3; ++r) {
      vertex.position[r] = vertex.normal[r] = 0;
      for (int c = 0; c < 3; ++c) {
        vertex.position[r] += m_inverse[r][c] * (input.position[c] - m_matrix[c][3]);
        vertex.normal[r] += m_matrix[c][r] * input.normal[c];
      }
    }
    normalize(vertex.normal);
    return vertex;
  }

private:
  static std::array<float, 3> cross(const std::array<float, 3> &a, const std::array<float, 3> &b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
  }
  static void normalize(float (&n)[3]) {
    const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (length > 1e-6f)
      for (float &c : n)
        c /= length;
  }
  float m_matrix[3][4]{}, m_inverse[3][3]{};
};

// clip(world triangle, pieces) returns true only when geometry was removed,
// not merely split by BSP planes. Untouched triangles retain their exact input
// attributes. Output contains only referenced vertices, with original winding.
template <typename Clip>
ClippedMesh clipPlacedMesh(const std::vector<Vertex> &vertices, const std::vector<uint32_t> &indices,
                           const float (&transform)[3][4], Clip &&clip) {
  if (indices.size() % 3)
    throw std::invalid_argument("Incomplete sky triangles");
  Placement placement(transform);
  ClippedMesh result;
  result.vertices.reserve(indices.size());
  result.indices.reserve(indices.size());
  std::vector<Polygon> pieces;
  for (size_t first = 0; first < indices.size(); first += 3) {
    Polygon triangle(3);
    for (int c = 0; c < 3; ++c)
      triangle[c] = placement.world(vertices.at(indices[first + c]));
    pieces.clear();
    const bool removed = clip(std::move(triangle), pieces);
    result.changed |= removed;
    if (!removed) {
      const auto base = uint32_t(result.vertices.size());
      for (int c = 0; c < 3; ++c)
        result.vertices.push_back(vertices.at(indices[first + c]));
      result.indices.insert(result.indices.end(), {base, base + 1, base + 2});
    } else
      for (auto &piece : pieces) {
        if (piece.size() < 3)
          continue;
        const auto base = uint32_t(result.vertices.size());
        for (auto &vertex : piece)
          result.vertices.push_back(placement.local(vertex));
        for (uint32_t c = 1; c + 1 < piece.size(); ++c)
          result.indices.insert(result.indices.end(), {base, base + c, base + c + 1});
      }
  }
  return result;
}
} // namespace proxy::sky
