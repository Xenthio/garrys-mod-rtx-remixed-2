#pragma once
#include <windows.h>
#include <array>

namespace proxy::diag {
// Engine import/vtable hooks can remain reachable after shader startup fails.
// Pin before publishing those pointers; Windows owns the final process teardown.
inline bool retainHookModule(HMODULE module) {
  HMODULE pinned = nullptr;
  return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(module), &pinned) != 0;
}

// Explicitly stopped by IAppSystem::Shutdown, before engine objects disappear.
// No destructor joins: DLL detach runs under the loader lock. The pinned module
// also protects callbacks if a failed host never calls Shutdown.
class Workers {
public:
  Workers() = default;
  Workers(const Workers &) = delete;
  Workers &operator=(const Workers &) = delete;

  bool start(LPTHREAD_START_ROUTINE entry, void *parameter = nullptr) {
    if (m_stopped || m_count == m_threads.size()) return false;
    if (!m_stop) m_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_stop) return false;
    const auto thread = CreateThread(nullptr, 0, entry, parameter, 0, nullptr);
    if (!thread) return false;
    m_threads[m_count++] = thread;
    return true;
  }
  bool wait(DWORD milliseconds) const {
    return !m_stop || WaitForSingleObject(m_stop, milliseconds) != WAIT_TIMEOUT;
  }
  bool stopping() const { return wait(0); }

  // Call outside DllMain and outside a managed worker.
  void stop() {
    m_stopped = true;
    if (m_stop) SetEvent(m_stop);
    for (size_t i = 0; i < m_count; ++i) {
      WaitForSingleObject(m_threads[i], INFINITE);
      CloseHandle(m_threads[i]);
    }
    m_count = 0;
    if (m_stop) CloseHandle(m_stop);
    m_stop = nullptr;
  }

private:
  HANDLE m_stop = nullptr;
  std::array<HANDLE, 3> m_threads{};
  size_t m_count = 0;
  bool m_stopped = false;
};
} // namespace proxy::diag
