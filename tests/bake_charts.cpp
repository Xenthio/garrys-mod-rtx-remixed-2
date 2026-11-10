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
  const std::vector<uint32_t> roundedIndices{0, 1, 2, 0, 2, 3};
  const auto repaired = lm::captureFan(rounded, 0, roundedIndices, 0, fan);
  require(repaired.size() == 1 && repaired[0].firstIndex == 3 && repaired[0].texcoords.size() == 6 &&
              roundedIndices == std::vector<uint32_t>({0, 1, 2, 0, 2, 3}),
          "Rounded slivers keep their geometry while the adjoining valid fan range gains a chart");
  lm::Mapping ceiling{{}, {}, {11, 10}};
  const std::vector<std::array<float, 2>> ceilingUv{
      {6.77745774f, 9.23830203f}, {7.28228309f, 8.49088755f},  {7.35440099f, 8.38411405f},
      {9.37370171f, 5.39445794f}, {10.14324737f, 3.28014696f}, {3.91778142f, 1.01426442f},
      {2.62570358f, .54398692f},  {.31706855f, 6.88691449f},   {3.95837524f, 8.21224071f}};
  std::vector<uint32_t> ceilingIndices;
  for (uint32_t i = 1; i + 1 < ceilingUv.size(); ++i)
    ceilingIndices.insert(ceilingIndices.end(), {0, i, i + 1});
  require(!lm::capture(ceiling, 0, ceilingIndices, 0, ceilingUv),
          "Construct ceiling face 7438 reproduces whole-face chart refusal before repair");
  const auto ceilingRanges = lm::captureFan(ceiling, 0, ceilingIndices, 0, ceilingUv);
  size_t covered = 0;
  for (const auto &range : ceilingRanges)
    covered += range.texcoords.size() / 2;
  require(!ceilingRanges.empty() && covered >= ceilingIndices.size() - 9,
          "Construct ceiling preserves its nondegenerate surface mapping");
  const auto totalArea = [](const std::vector<float> &corners) {
    double total = 0;
    for (size_t i = 0; i < corners.size(); i += 6)
      total += std::abs((double(corners[i + 2]) - corners[i]) * (double(corners[i + 5]) - corners[i + 1]) -
                        (double(corners[i + 3]) - corners[i + 1]) * (double(corners[i + 4]) - corners[i]));
    return total * .5;
  };
  std::vector<float> originalCeiling;
  for (uint32_t index : ceilingIndices)
    originalCeiling.insert(originalCeiling.end(), ceilingUv[index].begin(), ceilingUv[index].end());
  double recoveredArea = 0;
  for (const auto &range : ceilingRanges)
    recoveredArea += totalArea(range.texcoords);
  require(recoveredArea >= totalArea(originalCeiling) * .999999,
          "Construct repair retains more than 99.9999 percent of the face's original UV area");
  const std::vector<std::array<float, 2>> tinyRun{{0, 0}, {1, 0}, {2, .000001f}, {3, 0}, {0, 4}};
  const auto withoutTinyRun = lm::captureFan(rounded, 0, {0, 1, 2, 0, 2, 3, 0, 3, 4}, 0, tinyRun);
  require(withoutTinyRun.size() == 1 && withoutTinyRun[0].firstIndex == 6 &&
              withoutTinyRun[0].texcoords.size() == 6,
          "Repair never publishes an isolated nearly collinear chart before a reversed sliver");
  const std::vector<std::array<float, 2>> folded{{0, 0}, {4, 0}, {0, 4}, {4, 4}};
  require(lm::captureFan(rounded, 0, roundedIndices, 0, folded).empty(),
          "Substantial folded triangles still refuse the face instead of silently dropping area");
  require(!lm::capture(rounded, 0, roundedIndices, 0, fan, 99),
          "Partial chart ranges cannot leave their geometry indices");
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
              gridUv.front() == std::array<float, 2>{0, 0} && gridUv.back() == std::array<float, 2>{13, 10},
          "The complete displacement grid has bounded nonfolded UVs independent of brush projection");
  mapping.size[0] = 0;
  require(!lm::capture(mapping, 3, indices, 7, uv), "Degenerate charts keep probe fallback");
  mapping.size[0] = 4096;
  require(!lm::capture(mapping, 3, indices, 7, uv), "Chart dimensions obey native descriptor limits");
  std::puts("BSP chart mapping, corner order, rounding and fallback validation passed");
}
