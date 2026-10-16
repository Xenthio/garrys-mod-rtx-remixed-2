#include "shader_bytecode.h"
#include <cstdio>
#include <cstring>

namespace {
struct Shader {
  // SM1 DEF includes immediate values that are not instruction tokens. Reading
  // it as a token stream can skip END and continue into unrelated stack memory.
  uint32_t words[8] = {0xfffe0101, 81, 0xa00f0000, 0x0f000000, 0, 0, 0, 0xffff};
  UINT queryBytes = sizeof(words), resultBytes = sizeof(words);
  HRESULT queryStatus = S_OK, resultStatus = S_OK;
  unsigned copies = 0;
  HRESULT GetFunction(void *destination, UINT *bytes) {
    if (!destination) {
      *bytes = queryBytes;
      return queryStatus;
    }
    ++copies;
    if (*bytes >= sizeof(words))
      std::memcpy(destination, words, sizeof(words));
    *bytes = resultBytes;
    return resultStatus;
  }
};
bool require(bool condition, const char *message) {
  if (!condition)
    std::fprintf(stderr, "%s\n", message);
  return condition;
}
} // namespace

int main() {
  using proxy::d3d::readShaderBytecode;
  Shader shader;
  const auto code = readShaderBytecode(&shader);
  if (!require(code && code->size() == 8 && (*code)[7] == 0xffff && shader.copies == 1,
               "Capture must use the shader-owned length, including SM1 immediates"))
    return 1;
  if (!require(!readShaderBytecode<Shader>(nullptr), "Null shader must be refused"))
    return 1;
  for (UINT bytes : {0u, 4u, 9u, 4u * 1024 * 1024 + 4}) {
    Shader invalid;
    invalid.queryBytes = bytes;
    if (!require(!readShaderBytecode(&invalid) && invalid.copies == 0,
                 "Invalid lengths must be refused before copying"))
      return 1;
  }
  Shader failed;
  failed.queryStatus = E_FAIL;
  if (!require(!readShaderBytecode(&failed) && failed.copies == 0, "Query failure must be refused"))
    return 1;
  failed = {};
  failed.resultStatus = E_FAIL;
  if (!require(!readShaderBytecode(&failed), "Copy failure must be refused"))
    return 1;
  for (UINT bytes : {0u, 7u, 36u}) {
    Shader changed;
    changed.resultBytes = bytes;
    if (!require(!readShaderBytecode(&changed), "Invalid returned length must be refused"))
      return 1;
  }
  Shader smaller;
  smaller.resultBytes = 8;
  if (!require(readShaderBytecode(&smaller)->size() == 2, "A shorter returned function must trim the copy"))
    return 1;
  std::puts("Bounded shader capture passed");
  return 0;
}
