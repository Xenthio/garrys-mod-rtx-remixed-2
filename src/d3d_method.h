#pragma once

#include <windows.h>
#include <mutex>
#include <unordered_map>

namespace proxy::d3d {
namespace detail {
inline std::mutex resourcePatchMutex;
}
// Resources from different D3D9 pools need not share implementations. Keep the
// original method for each patched vtable, and never patch one table twice.
template <class Function> class ResourceMethod {
public:
  bool install(void **table, unsigned slot, Function replacement) {
    // VirtualProtect operates on a page, which can contain several vtables.
    // Serialize writes across method types as well as within this registry.
    std::scoped_lock lock(m_mutex, detail::resourcePatchMutex);
    if (m_original.find(table) != m_original.end())
      return true;
    DWORD protection;
    if (!VirtualProtect(&table[slot], sizeof(void *), PAGE_READWRITE, &protection))
      return false;
    m_original.emplace(table, reinterpret_cast<Function>(table[slot]));
    table[slot] = reinterpret_cast<void *>(replacement);
    DWORD ignored;
    VirtualProtect(&table[slot], sizeof(void *), protection, &ignored);
    return true;
  }

  Function original(void *object) {
    const auto table = *static_cast<void ***>(object);
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_original.at(table);
  }

private:
  std::mutex m_mutex;
  std::unordered_map<void **, Function> m_original;
};
} // namespace proxy::d3d
