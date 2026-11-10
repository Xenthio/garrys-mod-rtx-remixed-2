#include "diagnostic_workers.h"
#include <atomic>

namespace {
HMODULE module = nullptr;
proxy::diag::Workers workers;
std::atomic_uint retired{0};
DWORD WINAPI work(void *) {
  workers.wait(10000);
  ++retired;
  return 0;
}
}
extern "C" __declspec(dllexport) BOOL beginWorkers() {
  return proxy::diag::retainHookModule(module) && workers.start(work) && workers.start(work);
}
extern "C" __declspec(dllexport) unsigned stopWorkers() {
  workers.stop();
  return retired;
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) module = instance;
  return TRUE;
}
