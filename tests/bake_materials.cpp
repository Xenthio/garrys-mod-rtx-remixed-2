#include "bake_materials.h"
#include <cstdio>
#include <map>

namespace {
struct Geometry { std::vector<int> vertices, indices; };
struct Group { bool sky = false; std::map<uint32_t, Geometry> surfaces; };
struct Part { int material; std::vector<int> indices; };
struct Model { std::vector<Part> parts; };
struct Prop { bool skybox; const Model *model; };
struct Material { bool resolved; };
struct World {
  std::map<int, Group> groups, overlayGroups, layerGroups;
  std::vector<Prop> props;
};
}

int main() {
  World world;
  world.groups[0].surfaces[0] = {{0, 1, 2}, {0, 1, 2}};
  world.groups[0].surfaces[1] = {}; // unused material metadata creates no surface
  world.overlayGroups[0].surfaces[0] = {{0, 1, 2}, {0, 1, 2}};
  world.overlayGroups[0].surfaces[5] = {{0, 1, 2}, {0, 1, 2}};
  world.layerGroups[0].surfaces[2] = {{0, 1, 2}, {0, 1, 2}};
  world.groups[1].sky = true;
  world.groups[1].surfaces[3] = {{0, 1, 2}, {0, 1, 2}};
  Model sky{{{3, {0, 1, 2}}}}, prop{{{4, {0, 1, 2}}, {1, {}}}};
  world.props = {{true, &sky}, {false, &prop}};
  const auto required = proxy::bsp::bakeMaterialDependencies(world);
  if (required != std::vector<uint32_t>{0, 2, 4, 5})
    return 1;
  std::vector<Material> materials = {{true}, {false}, {true}, {false}, {true}, {true}};
  if (proxy::bsp::firstUnreadyBakeMaterial(required, materials)) {
    std::fprintf(stderr, "Unused and engine sky materials must not block static inputs\n");
    return 1;
  }
  for (uint32_t index : required) {
    materials[index].resolved = false;
    if (proxy::bsp::firstUnreadyBakeMaterial(required, materials) != index)
      return 1;
    materials[index].resolved = true;
  }
  materials.resize(4);
  if (proxy::bsp::firstUnreadyBakeMaterial(required, materials) != 4)
    return 1;
  materials.push_back({true});
  materials.push_back({true});
  world.props.push_back({false, nullptr});
  if (proxy::bsp::firstUnreadyBakeMaterial(proxy::bsp::bakeMaterialDependencies(world), materials) != UINT32_MAX)
    return 1;
  std::puts("Retained brush, overlay, terrain and prop material dependencies passed");
  return 0;
}
