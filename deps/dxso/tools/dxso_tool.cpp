// dxso_tool <shader.bin> <out.spv>: compiles a D3D9 shader (vs/ps 1.x-3.0
// bytecode) to SPIR-V with the standalone dxso and prints its interface.
#include "dxso/dxso_module.h"
#include "dxso/dxso_modinfo.h"
#include "d3d9/d3d9_caps.h"
#include <cstdio>
#include <fstream>
#include <vector>

int main(int argc, char **argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: dxso_tool <shader.bin> <out.spv>\n"); return 2; }
  std::ifstream in(argv[1], std::ios::binary | std::ios::ate);
  if (!in) { std::fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
  std::vector<char> data(size_t(in.tellg()));
  in.seekg(0); in.read(data.data(), data.size());
  try {
    dxvk::DxsoReader reader(data.data());
    dxvk::DxsoModule module(reader);
    dxvk::DxsoModuleInfo info = {};
    const auto analysis = module.analyze();
    const bool vertex = module.info().type() == dxvk::DxsoProgramTypes::VertexShader;
    dxvk::D3D9ConstantLayout layout = {};
    layout.floatCount = vertex ? dxvk::caps::MaxFloatConstantsVS : dxvk::caps::MaxFloatConstantsPS;
    layout.intCount = dxvk::caps::MaxOtherConstants;
    layout.boolCount = dxvk::caps::MaxOtherConstants;
    layout.bitmaskCount = 1;
    auto shader = module.compile(info, argv[1], analysis, layout);
    const auto &code = shader->code();
    std::ofstream(argv[2], std::ios::binary).write(reinterpret_cast<const char *>(code.data()), code.size());
    std::printf("%s %u.%u: %zu bytes of SPIR-V, inputs %08x outputs %08x, push %u bytes, samplers %04x, bindings:",
                vertex ? "vs" : "ps", module.info().majorVersion(), module.info().minorVersion(), code.size(),
                shader->info().inputMask, shader->info().outputMask, shader->info().pushConstSize, module.usedSamplers());
    for (const auto &b : shader->bindings())
      std::printf(" %u:%s", b.resourceBinding, b.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ? "ubo"
                                                 : b.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ? "ssbo"
                                                 : b.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ? "sampler" : "?");
    std::printf("\n");
  } catch (const dxvk::DxvkError &e) {
    std::fprintf(stderr, "dxso: %s\n", e.message().c_str());
    return 1;
  }
  return 0;
}
