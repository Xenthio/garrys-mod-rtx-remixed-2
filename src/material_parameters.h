#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace proxy::engine_materials {
// Parameter names belong to the material layout; types and values remain live.
// The owning Entry discards this cache when its shader/parameter layout changes.
class ParameterNames {
public:
  template <class NameAt>
  const std::vector<uint16_t> &find(const char *name, uint16_t count, const NameAt &nameAt) {
    if (count != m_count) {
      m_queries.clear();
      m_count = count;
    }
    for (const auto &query : m_queries)
      if (_stricmp(query.name.c_str(), name) == 0)
        return query.indices;
    Query query{name, {}};
    bool complete = true;
    for (uint16_t index = 0; index < count; ++index) {
      const char *candidate = nameAt(index);
      if (!candidate) { complete = false; continue; }
      if (_stricmp(candidate, name) == 0)
        query.indices.push_back(index);
    }
    // A foreign or temporarily unavailable variable may become readable later.
    // Preserve the current partial result without caching a permanent miss.
    if (!complete) {
      m_pending = std::move(query.indices);
      return m_pending;
    }
    m_queries.push_back(std::move(query));
    return m_queries.back().indices;
  }

private:
  struct Query {
    std::string name;
    std::vector<uint16_t> indices;
  };
  uint16_t m_count = 0;
  std::vector<Query> m_queries;
  std::vector<uint16_t> m_pending;
};
} // namespace proxy::engine_materials
