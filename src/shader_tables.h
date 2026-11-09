#pragma once

#include "d3d_hooks.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// A shader's constant table: the CTAB comment fxc leaves in the bytecode
// (D3DXSHADER_CONSTANTTABLE), which names each constant and sampler with its
// register. Source's shaders keep theirs (GMod's and L4D2's), so the
// registers a draw's shader reads can be looked up by name instead of
// assumed from one game's common_vs_fxc.h (L4D2's cFlexScale is c13, GMod's
// c3), and a shader can be recognised by what it declares.
namespace proxy::shader_tables {

struct ConstantTable {
  struct Entry {
    std::string name;
    uint16_t set, index, count; // set: D3DXRS_BOOL 0, INT4 1, FLOAT4 2, SAMPLER 3
  };
  std::vector<Entry> entries;
  const Entry *find(const char *name) const {
    for (const auto &e : entries)
      if (e.name == name)
        return &e;
    return nullptr;
  }
  int floatRegister(const char *name) const {
    const Entry *e = find(name);
    return e && e->set == 2 ? int(e->index) : -1;
  }
  int sampler(const char *name) const {
    const Entry *e = find(name);
    return e && e->set == 3 ? int(e->index) : -1;
  }
};

// False: the bytecode has no constant table (`out` then empty).
bool parse(const std::vector<uint32_t> &code, ConstantTable &out);

// What `describe` makes of each shader object's bytecode, kept per object.
// The bytecode is the identity: a shader created later at a freed one's
// address has new code, and is described anew. Render thread.
template <typename Info> struct Cached {
  std::shared_ptr<const std::vector<uint32_t>> code;
  Info info;
};
template <typename Info> using Cache = std::unordered_map<const void *, Cached<Info>>;

// Null: no shader, or its bytecode was not seen at creation.
template <typename Info>
const Info *lookup(Cache<Info> &cache, const void *shader, Info (*describe)(const std::vector<uint32_t> &)) {
  if (!shader)
    return nullptr;
  auto code = d3d::shaderCode(shader);
  if (!code)
    return nullptr;
  auto it = cache.find(shader);
  if (it == cache.end() || it->second.code != code) {
    if (cache.size() > 16384)
      cache.clear();
    Cached<Info> entry;
    entry.info = describe(*code);
    entry.code = std::move(code);
    it = cache.insert_or_assign(shader, std::move(entry)).first;
  }
  return &it->second.info;
}

} // namespace proxy::shader_tables
