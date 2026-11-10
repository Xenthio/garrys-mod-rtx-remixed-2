#include "sky_mesh_clip.h"
#include <cstdio>
#include <cstring>
#include <limits>
using namespace proxy::sky;
using SkyPolygon = proxy::sky::Polygon;
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
bool clipX(SkyPolygon input, std::vector<SkyPolygon> &pieces) {
  SkyPolygon kept;
  bool removed = false;
  for (size_t i = 0; i < input.size(); ++i) {
    const auto &a = input[i], &b = input[(i + 1) % input.size()];
    const bool inside = a.position[0] >= 0, next = b.position[0] >= 0;
    if (inside)
      kept.push_back(a);
    else
      removed = true;
    if (inside != next) {
      const float t = a.position[0] / (a.position[0] - b.position[0]);
      Vertex v = a;
      for (int c = 0; c < 3; ++c) {
        v.position[c] += (b.position[c] - a.position[c]) * t;
        v.normal[c] += (b.normal[c] - a.normal[c]) * t;
      }
      for (int c = 0; c < 2; ++c)
        v.texcoord[c] += (b.texcoord[c] - a.texcoord[c]) * t;
      v.color = 0xff556677;
      kept.push_back(v);
    }
  }
  if (kept.size() >= 3)
    pieces.push_back(std::move(kept));
  return removed;
}
int main() try {
  std::vector<Vertex> vertices(3);
  const float positions[3][3] = {{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}};
  for (int i = 0; i < 3; ++i) {
    std::copy_n(positions[i], 3, vertices[i].position);
    vertices[i].normal[2] = 1;
    vertices[i].texcoord[0] = (positions[i][0] + 1) * .5f;
    vertices[i].texcoord[1] = (positions[i][1] + 1) * .5f;
    vertices[i].color = 0xff123456;
  }
  for (float origin : {-8.f, 8.f}) {
    for (bool reversed : {false, true}) {
      // Rotation plus nonuniform scale: the returned mesh must remain local.
      const float transform[3][4] = {{16, 0, 0, origin}, {0, 0, -4, 20}, {0, 3, 0, -7}};
      Placement placement(transform);
      const std::vector<uint32_t> indices =
          reversed ? std::vector<uint32_t>{2, 1, 0} : std::vector<uint32_t>{0, 1, 2};
      const auto result = clipPlacedMesh(vertices, indices, transform, clipX);
      require(result.changed && !result.indices.empty(), "Partial sky card survives either origin side");
      bool boundary = false;
      for (const auto &v : result.vertices) {
        const auto p = placement.world(v);
        require(p.position[0] >= -.0001f, "No kept triangle enters the playable half space");
        boundary |= std::abs(p.position[0]) < .0001f;
        require(std::abs(v.texcoord[0] - (v.position[0] + 1) * .5f) < .0001f &&
                    std::abs(v.texcoord[1] - (v.position[1] + 1) * .5f) < .0001f,
                "Clipped UVs retain their model-space interpolation");
        const auto expectedColor = std::abs(p.position[0]) < .0001f ? 0xff556677u : 0xff123456u;
        require(v.color == expectedColor && std::abs(v.normal[2] - 1) < .0001f,
                "Original and newly interpolated colours/normals survive clipping");
      }
      require(boundary, "Crossing card is clipped at the actual boundary");
      for (size_t i = 0; i < result.indices.size(); i += 3) {
        const auto &a = result.vertices[result.indices[i]], &b = result.vertices[result.indices[i + 1]],
                   &c = result.vertices[result.indices[i + 2]];
        const float winding = (b.position[0] - a.position[0]) * (c.position[1] - a.position[1]) -
                              (b.position[1] - a.position[1]) * (c.position[0] - a.position[0]);
        require(reversed ? winding < 0 : winding > 0, "Triangle winding survives both transform and clip");
      }
    }
  }
  const float outside[3][4] = {{1, 0, 0, 10}, {0, 1, 0, 0}, {0, 0, 1, 0}};
  vertices[0].normal[2] = 2;
  const auto untouched = clipPlacedMesh(vertices, {0, 1, 2}, outside,
                                        [](SkyPolygon, std::vector<SkyPolygon> &) { return false; });
  require(!untouched.changed && untouched.vertices.size() == 3 &&
              std::memcmp(untouched.vertices.data(), vertices.data(), vertices.size() * sizeof(Vertex)) == 0,
          "Partition-only splits preserve exact original attributes");
  const auto removed = clipPlacedMesh(vertices, {0, 1, 2}, outside,
                                      [](SkyPolygon, std::vector<SkyPolygon> &) { return true; });
  require(removed.changed && removed.indices.empty() && removed.vertices.empty(),
          "Fully removed sky card has no upload");
  bool rejected = false;
  try {
    clipPlacedMesh(vertices, {0, 1}, outside, clipX);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected, "Incomplete triangles are refused");
  float invalid[3][4]{};
  rejected = false;
  try {
    Placement p(invalid);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected, "Singular transforms are refused");
  invalid[0][0] = std::numeric_limits<float>::quiet_NaN();
  rejected = false;
  try {
    Placement p(invalid);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected, "Non-finite transforms are refused");
  std::puts("Placed sky mesh clipping preserves boundaries, winding and attributes");
} catch (const std::exception &e) {
  std::fprintf(stderr, "%s\n", e.what());
  return 1;
}
