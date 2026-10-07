// shaderapidx9.dll proxy for Garry's Mod (x64): loads the original as
// shaderapidx9_orig.dll and hands out forwarding wrappers for its interfaces.
// Phase 1: pure pass-through with per-slot call statistics.
#include "d3d_hooks.h"
#include "engine_lights.h"
#include "engine_materials.h"
#include "engine_patches.h"
#include "engine_shadows.h"
#include "remix_output.h"
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
  if (!proxy::d3d::install(g_orig))
    log("proxy: d3d9!Direct3DCreate9 import not found; D3D hooks inactive");
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

// IShaderDeviceMgr slot 4: void Shutdown() (IAppSystem). The material system
// shuts the shader device down on quit, on the main thread: OpenRemix stops
// here rather than in exit-time teardown.
void deviceMgrShutdown(proxy::Wrapper *self) {
  using Fn = void (*)(void *);
  log("proxy: IShaderDeviceMgr::Shutdown");
  proxy::original<Fn>(self, 4)(self->real);
  proxy::remix::shutdown();
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

// IShaderAPI (ShaderApi030) slot 127: void SetLight(int lightNum,
// const LightDesc_t &desc) (copies 88 bytes into its light array). SDK 2013
// sets the model lights one at a time (L4D2 passes an array).
void shaderApiSetLight(proxy::Wrapper *self, int index, const void *desc) {
  using Fn = void (*)(void *, int, const void *);
  proxy::remix::onSetLights(1, desc);
  proxy::original<Fn>(self, 127)(self->real, index, desc);
}

// IShaderAPI slot 114: void Bind(IMaterial *pMaterial): the material of the
// draws that follow (those buffered for the previous one are flushed inside).
// Hooked only when engine_materials validated the slot.
void shaderApiBind(proxy::Wrapper *self, void *material) {
  using Fn = void (*)(void *, void *);
  proxy::original<Fn>(self, 114)(self->real, material);
  proxy::engine_materials::onBind(material);
}

// IShaderAPI slot 216: void SetPIXMarker(unsigned long color, const char *name)
// (an empty stub in release builds). Markers "openremix:..." are ours.
void shaderApiSetPIXMarker(proxy::Wrapper *self, unsigned long color, const char *name) {
  using Fn = void (*)(void *, unsigned long, const char *);
  if (proxy::remix::onMarker(name))
    return;
  proxy::original<Fn>(self, 216)(self->real, color, name);
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
       proxy::setHook(k, 4, reinterpret_cast<void *>(&deviceMgrShutdown));
       proxy::setHook(k, 12, reinterpret_cast<void *>(&deviceMgrSetMode));
     }},
    {"ShaderApi030",
     [](proxy::Kind *k) {
       proxy::setHook(k, 127, reinterpret_cast<void *>(&shaderApiSetLight));
       proxy::setHook(k, 216, reinterpret_cast<void *>(&shaderApiSetPIXMarker));
       if (proxy::engine_materials::install(k->realVtbl))
         proxy::setHook(k, 114, reinterpret_cast<void *>(&shaderApiBind));
     }},
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
    if (std::strcmp(name, "ShaderApi030") == 0)
      proxy::remix::setShaderApi(real);
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
// thread for 5 s into shaderapi_proxy_profile.txt as
// "<samples> <thread> <ip> [< [rsp] <code address at the stack top>]".
std::string symbolAt(DWORD64 pc) {
  HMODULE module = nullptr;
  char name[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(uintptr_t(pc)), &module))
    GetModuleFileNameA(module, name, MAX_PATH);
  const char *base = std::strrchr(name, '\\');
  char out[MAX_PATH + 32];
  std::snprintf(out, sizeof(out), "%s+0x%llx", base ? base + 1 : name,
                static_cast<unsigned long long>(module ? pc - uintptr_t(module) : pc));
  return out;
}

void sampleThreads() {
  std::map<std::pair<DWORD, std::string>, unsigned> counts;
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
      if (SuspendThread(thread) == DWORD(-1))
        continue;
      // While suspended: no allocation, no locks (the thread may hold the
      // heap or loader lock). Instruction pointer and the qword at RSP: the
      // return address when the thread is in a leaf function (memcpy).
      DWORD64 pc = 0, top[8] = {};
      if (GetThreadContext(thread, &context)) {
        pc = context.Rip;
        MEMORY_BASIC_INFORMATION stack = {};
        if (VirtualQuery(reinterpret_cast<void *>(context.Rsp), &stack, sizeof(stack)) && stack.State == MEM_COMMIT &&
            context.Rsp + sizeof(top) <= uintptr_t(stack.BaseAddress) + stack.RegionSize)
          std::memcpy(top, reinterpret_cast<void *>(context.Rsp), sizeof(top));
      }
      ResumeThread(thread);
      if (!pc)
        continue;
      // Caller: the first code address in another module among the top stack
      // slots (leaf functions that saved registers keep it a few slots up).
      std::string chain = symbolAt(pc);
      const auto own = codeRangeOf(reinterpret_cast<void *>(pc));
      for (DWORD64 v : top) {
        const auto range = codeRangeOf(reinterpret_cast<void *>(v));
        if (range.first && range != own) {
          chain += " < " + symbolAt(v);
          break;
        }
      }
      ++counts[{id, chain}];
    }
    Sleep(1);
  }
  for (auto [id, thread] : threads)
    CloseHandle(thread);
  FILE *f = _wfopen((g_dir + L"\\shaderapi_proxy_profile.txt").c_str(), L"w");
  if (!f)
    return;
  for (const auto &[key, count] : counts)
    fprintf(f, "%u %lu %s\n", count, key.first, key.second.c_str());
  fclose(f);
}

// Dev aid: `shaderapi_proxy_close.request` closes the game's windows
// (WM_CLOSE, as the window's close button; GMod blocks `quit` from Lua).
BOOL CALLBACK closeWindow(HWND window, LPARAM) {
  DWORD process = 0;
  GetWindowThreadProcessId(window, &process);
  if (process == GetCurrentProcessId() && IsWindowVisible(window))
    PostMessageW(window, WM_CLOSE, 0, 0);
  return TRUE;
}

DWORD WINAPI profilerThread(void *) {
  const std::wstring request = g_dir + L"\\shaderapi_proxy_profile.request";
  const std::wstring close = g_dir + L"\\shaderapi_proxy_close.request";
  for (;;) {
    Sleep(250);
    if (GetFileAttributesW(close.c_str()) != INVALID_FILE_ATTRIBUTES) {
      DeleteFileW(close.c_str());
      EnumWindows(closeWindow, 0);
    }
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
      proxy::d3d::dumpStatistics(f);
      proxy::engine_patches::dumpStatistics(f);
      proxy::engine_lights::dumpStatistics(f);
      proxy::engine_shadows::dumpStatistics(f);
      proxy::engine_materials::dumpStatistics(f);
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

// DXVK puts D3DUSAGE_WRITEONLY dynamic buffers in device-local host-visible
// memory (the PCIe BAR). The game's dynamic meshes are written through the
// proxy's shadow at Unlock, ~2.7 MiB/frame on gm_construct: 14 ms at the
// ~200 MB/s of a GPU behind Thunderbolt, 0.35 ms into cached system memory.
// DXVK reads DXVK_CONFIG when the D3D9 device is created, after this.
static void preferCachedDxvkBuffers() {
  constexpr char kOption[] = "d3d9.cachedWriteOnlyBuffers";
  char current[1024] = {};
  const DWORD n = GetEnvironmentVariableA("DXVK_CONFIG", current, sizeof(current));
  if (n >= sizeof(current) || std::strstr(current, kOption))
    return;
  std::string value = current;
  if (!value.empty())
    value += ';';
  value += std::string(kOption) + " = True";
  SetEnvironmentVariableA("DXVK_CONFIG", value.c_str());
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_self = instance;
    DisableThreadLibraryCalls(instance);
    preferCachedDxvkBuffers();
  }
  return TRUE;
}
