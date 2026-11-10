#include "probe_receivers.h"
#include <remix/remix_native_ext.h>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace proxy::replay {
namespace {
void check(remixapi_ErrorCode result, const char *operation) {
  if (result != REMIXAPI_ERROR_CODE_SUCCESS)
    throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(unsigned(result)));
}
}
ProbeReceivers::ProbeReceivers(remixapi_Interface &api, const std::vector<std::array<float, 4>> &positions,
                               float roughness, float metallic, uint32_t segments)
    : api(api), positions(positions) {
  if (positions.empty()) return;
  if (segments < 8 || segments > 256 || segments % 2)
    throw std::runtime_error("Receiver segments must be an even integer in [8,256]");
  try {
    remixapi_MaterialInfoOpaqueEXT opaque{};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant = {.5f, .5f, .5f};
    opaque.opacityConstant = 1;
    opaque.roughnessConstant = roughness;
    opaque.metallicConstant = metallic;
    opaque.alphaTestType = 7;
    remixapi_MaterialInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.pNext = &opaque;
    info.hash = 0x70726f62652d6d61ull;
    check(api.CreateMaterial(&info, &material), "Create diagnostic receiver material");
    const uint32_t rings = segments / 2;
    constexpr float pi = 3.14159265358979323846f;
    std::vector<remixapi_HardcodedVertex> vertices;
    std::vector<uint32_t> indices;
    for (uint32_t ring = 0; ring <= rings; ++ring) {
      const float latitude = pi * ring / rings;
      for (uint32_t segment = 0; segment <= segments; ++segment) {
        const float longitude = 2 * pi * segment / segments;
        remixapi_HardcodedVertex vertex{};
        vertex.normal[0] = vertex.position[0] = std::sin(latitude) * std::cos(longitude);
        vertex.normal[1] = vertex.position[1] = std::sin(latitude) * std::sin(longitude);
        vertex.normal[2] = vertex.position[2] = std::cos(latitude);
        vertex.color = 0xffffffff;
        vertices.push_back(vertex);
      }
    }
    for (uint32_t ring = 0; ring < rings; ++ring)
      for (uint32_t segment = 0; segment < segments; ++segment) {
        const auto a = ring * (segments + 1) + segment, b = a + segments + 1;
        indices.insert(indices.end(), {a, b, a + 1, a + 1, b, b + 1});
      }
    remixapi_MeshInfoSurfaceTriangles surface{};
    surface.vertices_values = vertices.data(); surface.vertices_count = vertices.size();
    surface.indices_values = indices.data(); surface.indices_count = indices.size();
    surface.material = material;
    remixapi_MeshInfo geometry{};
    geometry.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    geometry.hash = 0x70726f62652d6d65ull ^ (uint64_t(segments ^ 32u) << 32);
    geometry.surfaces_values = &surface; geometry.surfaces_count = 1;
    check(api.CreateMesh(&geometry, &mesh), "Create diagnostic receiver sphere");
    std::printf("Diagnostic probe receivers: %zu dynamic spheres, roughness %.3f, metallic %.3f, %u rings x %u segments; excluded from static bake inputs\n",
                positions.size(), roughness, metallic, rings, segments);
  } catch (...) {
    release();
    throw;
  }
}
void ProbeReceivers::release() {
  if (mesh) api.DestroyMesh(mesh);
  if (material) api.DestroyMaterial(material);
  mesh = nullptr; material = nullptr;
}
ProbeReceivers::~ProbeReceivers() { release(); }
void ProbeReceivers::draw(const remixapi_CameraInfoParameterizedEXT *camera) {
  for (size_t i = 0; i < positions.size(); ++i) {
    remixapi_InstanceIdentityEXT identity{};
    identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
    identity.instanceId = 0x70726f6265000000ull + i;
    identity.classification = REMIXAPI_INSTANCE_DYNAMIC_EXT;
    identity.rasterVisible = 1;
    remixapi_InstanceInfo instance{};
    instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    instance.pNext = &identity; instance.mesh = mesh; instance.doubleSided = 1;
    auto position = positions[i];
    if (camera) {
      const auto &c = *camera;
      const float eye[3]{c.position.x, c.position.y, c.position.z};
      const float forward[3]{c.forward.x, c.forward.y, c.forward.z};
      const float right[3]{c.right.x, c.right.y, c.right.z};
      const float up[3]{c.up.x, c.up.y, c.up.z};
      for (uint32_t axis = 0; axis < 3; ++axis)
        position[axis] = eye[axis] + positions[i][0] * forward[axis] +
                         positions[i][1] * right[axis] + positions[i][2] * up[axis];
    }
    for (uint32_t axis = 0; axis < 3; ++axis) {
      instance.transform.matrix[axis][axis] = positions[i][3];
      instance.transform.matrix[axis][3] = position[axis];
    }
    check(api.DrawInstance(&instance), "Draw diagnostic probe receiver");
  }
}
}
