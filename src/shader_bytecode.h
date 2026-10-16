#pragma once

#include <windows.h>
#include <cstdint>
#include <memory>
#include <vector>

namespace proxy::d3d {
// D3D9 creation inputs have no length. Ask the created shader for its owned
// bytecode instead of scanning caller memory for a potentially skipped END.
template <class Shader>
std::shared_ptr<const std::vector<uint32_t>> readShaderBytecode(Shader *shader) {
  constexpr UINT maximumBytes = 4 * 1024 * 1024;
  UINT bytes = 0;
  if (!shader || FAILED(shader->GetFunction(nullptr, &bytes)) || bytes < 8 ||
      bytes > maximumBytes || bytes % sizeof(uint32_t))
    return {};
  auto code = std::make_shared<std::vector<uint32_t>>(bytes / sizeof(uint32_t));
  const UINT capacity = bytes;
  if (FAILED(shader->GetFunction(code->data(), &bytes)) || bytes > capacity || bytes < 8 ||
      bytes % sizeof(uint32_t))
    return {};
  code->resize(bytes / sizeof(uint32_t));
  return code;
}
} // namespace proxy::d3d
