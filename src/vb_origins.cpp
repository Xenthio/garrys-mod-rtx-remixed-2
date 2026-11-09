#include "vb_origins.h"
#include "game.h"
#include "vtable_proxy.h"
#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace proxy::vb_origins {
namespace {

std::mutex g_lock;
std::unordered_map<const void *, Origin> g_buffers;
std::set<std::string> g_names; // interned allocator names (stable c_str)
// The innermost open TrackMeshAllocations bracket of each thread.
thread_local const char *t_allocator = nullptr;
// Unlit draws by origin since the last write: (allocator, format, vertex size, dynamic, tracked).
using Key = std::tuple<std::string, uint64_t, int, bool, bool>;
std::map<Key, uint64_t> g_unlit;

const char *intern(const char *name) {
  if (!name)
    return nullptr;
  std::lock_guard lock(g_lock);
  return g_names.insert(name).first->c_str();
}

// IVBAllocTracker slot 0: void CountVB(void *buffer, bool isDynamic,
// int bufferSize, int vertexSize, VertexFormat_t fmt).
void PROXY_THISCALL countVB(Wrapper *self, void *buffer, bool dynamic, int size, int vertexSize, uint64_t format) {
  using Fn = void(PROXY_THISCALL *)(void *, void *, bool, int, int, uint64_t);
  if (buffer) {
    std::lock_guard lock(g_lock);
    g_buffers[buffer] = {t_allocator, format, vertexSize, dynamic};
  }
  original<Fn>(self, 0)(self->real, buffer, dynamic, size, vertexSize, format);
}

// Slot 1: void UnCountVB(void *buffer).
void PROXY_THISCALL unCountVB(Wrapper *self, void *buffer) {
  using Fn = void(PROXY_THISCALL *)(void *, void *);
  {
    std::lock_guard lock(g_lock);
    g_buffers.erase(buffer);
  }
  original<Fn>(self, 1)(self->real, buffer);
}

// Slot 2: bool TrackMeshAllocations(const char *allocatorName); a null name
// closes the bracket.
bool PROXY_THISCALL trackMeshAllocations(Wrapper *self, const char *name) {
  using Fn = bool(PROXY_THISCALL *)(void *, const char *);
  t_allocator = intern(name);
  return original<Fn>(self, 2)(self->real, name);
}

} // namespace

void install(Kind *kind) {
  setHook(kind, 0, reinterpret_cast<void *>(&countVB));
  setHook(kind, 1, reinterpret_cast<void *>(&unCountVB));
  setHook(kind, 2, reinterpret_cast<void *>(&trackMeshAllocations));
}

bool find(const void *buffer, Origin &out) {
  std::lock_guard lock(g_lock);
  auto it = g_buffers.find(buffer);
  if (it == g_buffers.end())
    return false;
  out = it->second;
  return true;
}

void countUnlit(const void *buffer) {
  Origin origin;
  const bool tracked = find(buffer, origin);
  std::lock_guard lock(g_lock);
  ++g_unlit[{origin.allocator ? origin.allocator : "", origin.format, origin.vertexSize, origin.dynamic, tracked}];
}

void write(FILE *out, double frames) {
  std::vector<std::pair<uint64_t, Key>> rows;
  size_t buffers;
  {
    std::lock_guard lock(g_lock);
    for (const auto &[key, count] : g_unlit)
      rows.emplace_back(count, key);
    g_unlit.clear();
    buffers = g_buffers.size();
  }
  std::sort(rows.rbegin(), rows.rend());
  // Live buffers by creator, for telling which engine paths bracket theirs.
  std::map<std::pair<std::string, bool>, size_t> creators;
  {
    std::lock_guard lock(g_lock);
    for (const auto &[buffer, origin] : g_buffers)
      ++creators[{origin.allocator ? origin.allocator : "(no allocator bracket)", origin.dynamic}];
  }
  fprintf(out, "   vertex buffer origins: %zu tracked | unlit draws per frame by origin:", buffers);
  if (rows.empty())
    fprintf(out, " none");
  fprintf(out, "\n");
  fprintf(out, "     live buffers by creator:");
  for (const auto &[creator, count] : creators)
    fprintf(out, " %s%s %zu;", creator.first.c_str(), creator.second ? " (dynamic)" : "", count);
  fprintf(out, "\n");
  for (size_t i = 0; i < rows.size() && i < 8; ++i) {
    const auto &[name, format, size, dynamic, tracked] = rows[i].second;
    if (!tracked)
      fprintf(out, "     %.1f: buffer not reported by the tracker\n", double(rows[i].first) / std::max(frames, 1.0));
    else
      fprintf(out, "     %.1f: %s, format %016llx, %d B vertices%s\n", double(rows[i].first) / std::max(frames, 1.0),
              name.empty() ? "(no allocator bracket)" : name.c_str(), static_cast<unsigned long long>(format), size,
              dynamic ? ", dynamic" : "");
  }
}

} // namespace proxy::vb_origins
