#include "diagnostic_workers.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
struct Work {
  proxy::diag::Workers workers;
  std::atomic_uint entered{0}, retired{0};
  ~Work() { workers.stop(); }
};
DWORD WINAPI work(void *parameter) {
  auto &state = *static_cast<Work *>(parameter);
  ++state.entered;
  state.workers.wait(10000);
  ++state.retired;
  return 0;
}
}
int main(int argc, char **argv) try {
  require(argc == 2, "Expected fixture DLL path");
  Work state;
  require(state.workers.start(work, &state) && state.workers.start(work, &state), "Workers start");
  const auto deadline = GetTickCount64() + 2000;
  while (state.entered != 2 && GetTickCount64() < deadline) Sleep(1);
  const auto start = std::chrono::steady_clock::now();
  state.workers.stop();
  require(state.entered == 2 && state.retired == 2, "Stop joins both workers before returning");
  require(std::chrono::steady_clock::now() - start < std::chrono::seconds(2),
          "Stop interrupts long polling waits");
  state.workers.stop();
  require(!state.workers.start(work, &state), "A stopped group cannot publish new worker callbacks");

  const auto path = std::filesystem::absolute(argv[1]);
  const auto module = LoadLibraryW(path.c_str());
  require(module != nullptr, "Fixture DLL loads");
  const auto begin = reinterpret_cast<BOOL (*)()>(GetProcAddress(module, "beginWorkers"));
  const auto stop = reinterpret_cast<unsigned (*)()>(GetProcAddress(module, "stopWorkers"));
  require(begin && stop && begin(), "Fixture publishes pinned diagnostic workers");
  require(FreeLibrary(module) != 0, "Host releases its DLL ownership");
  HMODULE retained = nullptr;
  require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(begin), &retained) != 0,
          "Hook module stays mapped after the host releases it");
  require(stop() == 2, "Workers remain callable and join after host release");
  std::puts("Diagnostic workers: interruptible polling, joined shutdown and pinned hook lifetime passed");
} catch (const std::exception &error) {
  std::fprintf(stderr, "%s\n", error.what());
  return 1;
}
