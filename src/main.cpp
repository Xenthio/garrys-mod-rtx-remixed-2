// shaderapidx9.dll proxy for Garry's Mod (x64): loads the original as
// shaderapidx9_orig.dll and hands out forwarding wrappers for its interfaces.
// Phase 1: pure pass-through with per-slot call statistics.
#include "vtable_proxy.h"

#include <tlhelp32.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using CreateInterfaceFn = void *(*)(const char *name, int *returnCode);

HMODULE g_self = nullptr;
HMODULE g_orig = nullptr;
CreateInterfaceFn g_origCreateInterface = nullptr;
FILE *g_log = nullptr;
std::wstring g_dir;

void log(const char *fmt, ...) {
  if (!g_log)
    return;
  va_list args;
  va_start(args, fmt);
  vfprintf(g_log, fmt, args);
  va_end(args);
  fputc('\n', g_log);
  fflush(g_log);
}

// Executable range of the module containing `p` (empty if none).
std::pair<uintptr_t, uintptr_t> codeRangeOf(const void *p) {
  HMODULE m = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          static_cast<LPCWSTR>(p), &m))
    return {0, 0};
  auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(m);
  auto *nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(reinterpret_cast<uint8_t *>(m) + dos->e_lfanew);
  auto *sec = IMAGE_FIRST_SECTION(nt);
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
    if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) {
      const uintptr_t begin = reinterpret_cast<uintptr_t>(m) + sec->VirtualAddress;
      return {begin, begin + sec->Misc.VirtualSize};
    }
  }
  return {0, 0};
}

// Slots of a vtable: entries that point into the code of its first entry's module.
uint32_t countSlots(const void *const *vtbl) {
  const auto [begin, end] = codeRangeOf(vtbl[0]);
  uint32_t n = 0;
  while (n < 1024 && reinterpret_cast<uintptr_t>(vtbl[n]) >= begin && reinterpret_cast<uintptr_t>(vtbl[n]) < end)
    ++n;
  return n;
}

bool loadOriginal() {
  if (g_orig)
    return true;
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(g_self, path, MAX_PATH);
  g_dir = path;
  g_dir.resize(g_dir.find_last_of(L"\\/"));
  g_log = _wfopen((g_dir + L"\\shaderapi_proxy.log").c_str(), L"w");
  g_orig = LoadLibraryW((g_dir + L"\\shaderapidx9_orig.dll").c_str());
  if (!g_orig) {
    log("proxy: failed to load shaderapidx9_orig.dll (%lu)", GetLastError());
    return false;
  }
  g_origCreateInterface = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(g_orig, "CreateInterface"));
  log("proxy: loaded original at %p", static_cast<void *>(g_orig));
  return g_origCreateInterface != nullptr;
}

void *wrapInterface(const char *name, void *real);

// IAppSystem: Connect 0, Disconnect 1, QueryInterface 2, Init 3, Shutdown 4
// (GMod keeps the classic five, sourcesdk-minimal appframework/iappsystem.h).

// IShaderDeviceMgr slot 2: QueryInterface.
void *deviceMgrQueryInterface(proxy::Wrapper *self, const char *name) {
  using Fn = void *(*)(void *, const char *);
  void *real = proxy::original<Fn>(self, 2)(self->real, name);
  return wrapInterface(name, real);
}

// Factory the material system passes to IShaderDeviceMgr::Connect: the shader
// API fetches VShaderUtil001 through it, which we wrap.
CreateInterfaceFn g_connectFactory = nullptr;
void *connectFactory(const char *name, int *rc) {
  void *real = g_connectFactory ? g_connectFactory(name, rc) : nullptr;
  return wrapInterface(name, real);
}

// IShaderDeviceMgr slot 0: bool Connect(CreateInterfaceFn factory).
bool deviceMgrConnect(proxy::Wrapper *self, CreateInterfaceFn factory) {
  using Fn = bool (*)(void *, CreateInterfaceFn);
  g_connectFactory = factory;
  log("proxy: IShaderDeviceMgr::Connect(factory %p)", reinterpret_cast<void *>(factory));
  return proxy::original<Fn>(self, 0)(self->real, factory ? connectFactory : nullptr);
}

// IShaderDeviceMgr slot 12 (5 + 7): CreateInterfaceFn SetMode(void *hWnd,
// int nAdapter, const ShaderDeviceInfo_t &mode). The material system fetches
// the shader API interfaces through the factory it returns.
CreateInterfaceFn g_setModeFactory = nullptr;
void *setModeFactory(const char *name, int *rc) {
  void *real = g_setModeFactory ? g_setModeFactory(name, rc) : nullptr;
  return wrapInterface(name, real);
}
CreateInterfaceFn deviceMgrSetMode(proxy::Wrapper *self, void *hwnd, int adapter, const void *mode) {
  using Fn = CreateInterfaceFn (*)(void *, void *, int, const void *);
  CreateInterfaceFn factory = proxy::original<Fn>(self, 12)(self->real, hwnd, adapter, mode);
  log("proxy: SetMode(hwnd=%p, adapter=%d) -> factory %p", hwnd, adapter, reinterpret_cast<void *>(factory));
  if (!factory)
    return nullptr;
  g_setModeFactory = factory;
  return setModeFactory;
}

// IShaderUtil (material system) slot 12 (5 + 7): void RestoreShaderObjects(
// CreateInterfaceFn shaderFactory, int nChangeFlags). The material system
// refreshes its global shader API pointers from this factory.
CreateInterfaceFn g_restoreFactory = nullptr;
void *restoreFactory(const char *name, int *rc) {
  void *real = g_restoreFactory ? g_restoreFactory(name, rc) : nullptr;
  return wrapInterface(name, real);
}
void shaderUtilRestoreShaderObjects(proxy::Wrapper *self, CreateInterfaceFn factory, int flags) {
  using Fn = void (*)(void *, CreateInterfaceFn, int);
  log("proxy: IShaderUtil::RestoreShaderObjects(factory %p, flags %d)", reinterpret_cast<void *>(factory), flags);
  if (factory && factory != restoreFactory)
    g_restoreFactory = factory;
  proxy::original<Fn>(self, 12)(self->real, factory ? restoreFactory : nullptr, flags);
}

struct KnownInterface {
  const char *name;
  void (*setup)(proxy::Kind *);
};

const KnownInterface kKnown[] = {
    {"ShaderDeviceMgr001",
     [](proxy::Kind *k) {
       proxy::setHook(k, 0, reinterpret_cast<void *>(&deviceMgrConnect));
       proxy::setHook(k, 2, reinterpret_cast<void *>(&deviceMgrQueryInterface));
       proxy::setHook(k, 12, reinterpret_cast<void *>(&deviceMgrSetMode));
     }},
    {"ShaderApi030", nullptr},
    {"ShaderDevice001", nullptr},
    {"ShaderShadow010", nullptr},
    {"DebugTextureInfo001", nullptr},
    {"MaterialSystemHardwareConfig012", nullptr},
    {"VBAllocTracker001", nullptr},
    // Material system interface handed to the shader API.
    {"VShaderUtil001",
     [](proxy::Kind *k) { proxy::setHook(k, 12, reinterpret_cast<void *>(&shaderUtilRestoreShaderObjects)); }},
};

void *wrapInterface(const char *name, void *real) {
  if (!real || !name)
    return real;
  for (const auto &k : kKnown) {
    if (std::strcmp(k.name, name) != 0)
      continue;
    auto *vtbl = *static_cast<const void *const *const *>(real);
    if (codeRangeOf(vtbl[0]).first == 0) {
      log("proxy: %s at %p does not look like a module object", name, real);
      return real;
    }
    bool created = false;
    auto *kind = proxy::kindFor(name, vtbl, countSlots(vtbl), &created);
    if (created && k.setup)
      k.setup(kind);
    void *w = proxy::wrap(kind, real);
    log("proxy: %s real %p (vtable %p, %u slots) -> wrapper %p", name, real, static_cast<const void *>(vtbl),
        kind->slotCount, w);
    return w;
  }
  log("proxy: passing through unknown interface %s = %p", name, real);
  return real;
}

// Diagnostics: pointer-sized slots in other modules' writable sections that
// hold a real object we also handed out as a wrapper (references that bypass
// the proxy).
void scanForRealPointers() {
  const char *modules[] = {"materialsystem.dll", "stdshader_dx9.dll", "studiorender.dll", "engine.dll",
                           "vguimatsurface.dll", "client.dll"};
  const auto reals = proxy::realObjects();
  for (const char *name : modules) {
    HMODULE m = GetModuleHandleA(name);
    if (!m)
      continue;
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(m);
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(reinterpret_cast<uint8_t *>(m) + dos->e_lfanew);
    auto *sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
      if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE))
        continue;
      auto *p = reinterpret_cast<void **>(reinterpret_cast<uint8_t *>(m) + sec->VirtualAddress);
      const size_t n = sec->Misc.VirtualSize / sizeof(void *);
      for (size_t k = 0; k < n; ++k)
        for (auto &[real, iface] : reals)
          if (p[k] == real)
            log("scan: %s+%#llx holds real %s %p", name,
                static_cast<unsigned long long>(reinterpret_cast<uint8_t *>(&p[k]) - reinterpret_cast<uint8_t *>(m)),
                iface, real);
    }
  }
  log("scan: done");
}

// Dev aid: `shaderapi_proxy_profile.request` next to the DLL samples every
// thread's instruction pointer for 5 s into shaderapi_proxy_profile.txt as
// "<samples> <thread> <module>+<offset>", offsets rounded to 16 bytes.
void sampleThreads() {
  std::map<std::tuple<DWORD, std::string, uint32_t>, unsigned> counts;
  const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
  std::vector<std::pair<DWORD, HANDLE>> threads;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  THREADENTRY32 entry = {};
  entry.dwSize = sizeof(entry);
  for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry))
    if (entry.th32OwnerProcessID == pid && entry.th32ThreadID != self)
      if (HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, entry.th32ThreadID))
        threads.emplace_back(entry.th32ThreadID, thread);
  CloseHandle(snapshot);
  const ULONGLONG end = GetTickCount64() + 5000;
  while (GetTickCount64() < end) {
    for (auto [id, thread] : threads) {
      CONTEXT context = {};
      context.ContextFlags = CONTEXT_CONTROL;
      bool sampled = false;
      if (SuspendThread(thread) != DWORD(-1)) {
        sampled = GetThreadContext(thread, &context);
        ResumeThread(thread);
      }
      if (!sampled)
        continue;
      HMODULE module = nullptr;
      char name[MAX_PATH] = "?";
      if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCSTR>(uintptr_t(context.Rip)), &module))
        GetModuleFileNameA(module, name, MAX_PATH);
      const char *base = std::strrchr(name, '\\');
      const uint32_t offset =
          module ? uint32_t(context.Rip - uintptr_t(module)) & ~15u : uint32_t(context.Rip);
      ++counts[{id, base ? base + 1 : name, offset}];
    }
    Sleep(1);
  }
  for (auto [id, thread] : threads)
    CloseHandle(thread);
  FILE *f = _wfopen((g_dir + L"\\shaderapi_proxy_profile.txt").c_str(), L"w");
  if (!f)
    return;
  for (const auto &[key, count] : counts)
    fprintf(f, "%u %lu %s+0x%x\n", count, std::get<0>(key), std::get<1>(key).c_str(), std::get<2>(key));
  fclose(f);
}

DWORD WINAPI profilerThread(void *) {
  const std::wstring request = g_dir + L"\\shaderapi_proxy_profile.request";
  for (;;) {
    Sleep(250);
    if (GetFileAttributesW(request.c_str()) == INVALID_FILE_ATTRIBUTES)
      continue;
    DeleteFileW(request.c_str());
    sampleThreads();
  }
}

// Per-slot call counts to shaderapi_proxy_slots.txt every 10 s.
DWORD WINAPI statisticsThread(void *) {
  for (int tick = 0;; ++tick) {
    Sleep(10000);
    if (tick == 3)
      scanForRealPointers();
    FILE *f = _wfopen((g_dir + L"\\shaderapi_proxy_slots.txt").c_str(), L"w");
    if (f) {
      proxy::dumpStatistics(f);
      fclose(f);
    }
  }
}

} // namespace

// Marker for other modules: the original shader API is shaderapidx9_orig.dll.
extern "C" __declspec(dllexport) int openremix_shaderapi_proxy = 1;

extern "C" __declspec(dllexport) void *CreateInterface(const char *name, int *returnCode) {
  if (!loadOriginal()) {
    if (returnCode)
      *returnCode = 1; // IFACE_FAILED
    return nullptr;
  }
  static bool threadsStarted = false;
  if (!threadsStarted) {
    threadsStarted = true;
    CloseHandle(CreateThread(nullptr, 0, statisticsThread, nullptr, 0, nullptr));
    CloseHandle(CreateThread(nullptr, 0, profilerThread, nullptr, 0, nullptr));
  }
  return wrapInterface(name, g_origCreateInterface(name, returnCode));
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_self = instance;
    DisableThreadLibraryCalls(instance);
  }
  return TRUE;
}
