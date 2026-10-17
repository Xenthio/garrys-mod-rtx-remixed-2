#include "bake_charts.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
namespace lm = proxy::bsp::lightmaps;
void require(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
  }
}
int main() {
  lm::Mapping mapping{{{.25f, 0, 0, 1}, {0, .25f, 0, -2}}, {1, -2}, {4, 4}};
  std::vector<std::array<float, 2>> uv;
  for (const auto &point : std::vector<std::array<float, 3>>{{0, 0, 0}, {16, 0, 0}, {16, 16, 0}, {0, 16, 0}})
    uv.push_back(mapping.project(point.data()));
  std::vector<uint32_t> indices{0, 1, 2, 7, 9, 8, 7, 10, 9};
  auto chart = lm::capture(mapping, 3, indices, 7, uv);
  require(chart && chart->firstIndex == 3 && chart->width == 5 && chart->height == 5 &&
              chart->texcoords.size() == 12 && chart->texcoords[2] == 4 && chart->texcoords[3] == 4,
          "Face mapping preserves luxel centres, dimensions and indexed-corner order");
  uv[0][0] = -.005f;
  chart = lm::capture(mapping, 3, indices, 7, uv);
  require(chart && chart->texcoords[0] == 0, "Small BSP edge rounding clamps to the chart boundary");
  uv[0][0] = -.5f;
  require(!lm::capture(mapping, 3, indices, 7, uv), "Incorrect UVs cannot become chart data");
  uv[0][0] = std::numeric_limits<float>::quiet_NaN();
  require(!lm::capture(mapping, 3, indices, 7, uv), "Nonfinite mappings remain uncharted");
  uv[0][0] = 0;
  require(!lm::capture(mapping, 0, indices, 7, uv), "A chart cannot borrow earlier face vertices");
  require(!lm::capture(mapping, 4, indices, 7, uv), "Chart ranges must contain complete triangles");
  indices.back() = 100;
  require(!lm::capture(mapping, 3, indices, 7, uv), "Out-of-range vertex references are refused");
  indices.back() = 9;
  auto collapsed = uv;
  for (auto &coordinate : collapsed)
    coordinate = {.5f, .5f};
  require(!lm::capture(mapping, 3, indices, 7, collapsed),
          "A fully collapsed UV fan remains uncharted instead of invalidating the complete native bake");
  // Rounded fan from gm_flatgrass: the first sliver folds onto its neighbour.
  lm::Mapping rounded{{}, {}, {29, 29}};
  std::vector<std::array<float, 2>> fan{{28.8628998f, .117706299f},
                                        {22.9805603f, 6.00004196f},
                                        {.353118896f, 28.6274872f},
                                        {.353118896f, 14.3726273f}};
  require(!lm::capture(rounded, 0, {0, 1, 2, 0, 2, 3}, 0, fan),
          "Rounded folded fans stay on probes instead of invalidating a real map bake");
  require(lm::capture(rounded, 0, {0, 2, 3}, 0, fan).has_value(),
          "The adjoining nondegenerate face still has a valid chart");
  // Construct displacement face 9255 has a 13x10 luxel rectangle, while its
  // TexInfo projection reaches y=12.9375. Brush projection rejects the grid.
  lm::Mapping displaced{{}, {}, {13, 10}};
  const std::vector<std::array<float, 2>> brushUv{{0, 12.9375f}, {0, 0}, {9, 0}, {9, 12.9375f}};
  require(!lm::capture(displaced, 0, {0, 1, 2, 0, 2, 3}, 0, brushUv),
          "Displacement dimensions cannot be interpreted as brush projection bounds");
  std::vector<std::array<float, 2>> gridUv;
  for (uint32_t y = 0; y < 5; ++y)
    for (uint32_t x = 0; x < 5; ++x)
      gridUv.push_back(displaced.displacement(float(x) / 4, float(y) / 4));
  std::vector<uint32_t> gridIndices;
  for (uint32_t y = 0; y < 4; ++y)
    for (uint32_t x = 0; x < 4; ++x) {
      const uint32_t i = y * 5 + x;
      gridIndices.insert(gridIndices.end(), {i, i + 5, i + 6, i, i + 6, i + 1});
    }
  const auto displacement = lm::capture(displaced, 0, gridIndices, 0, gridUv);
  require(displacement && displacement->width == 14 && displacement->height == 11 &&
              displacement->texcoords.size() == gridIndices.size() * 2 &&
              gridUv.front() == std::array<float, 2>{0, 0} &&
              gridUv.back() == std::array<float, 2>{13, 10},
          "The complete displacement grid has bounded nonfolded UVs independent of brush projection");
  mapping.size[0] = 0;
  require(!lm::capture(mapping, 3, indices, 7, uv), "Degenerate charts keep probe fallback");
  mapping.size[0] = 4096;
  require(!lm::capture(mapping, 3, indices, 7, uv), "Chart dimensions obey native descriptor limits");
  std::puts("BSP chart mapping, corner order, rounding and fallback validation passed");
}
