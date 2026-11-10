// shaderapidx9.dll proxy for Garry's Mod and Team Fortress 2 (x64), Left 4
// Dead 2, Half-Life 2 RTX, Entropy : Zero 2 and Portal 2 (x86, see game.h):
// loads the original as
// shaderapidx9_orig.dll and hands out forwarding wrappers for its interfaces.
// Phase 1: pure pass-through with per-slot call statistics.
#include "diag.h"
#include "diagnostic_workers.h"
#include "d3d_hooks.h"
#include "game.h"
#include "engine_lights.h"
#include "engine_materials.h"
#include "engine_mirrors.h"
#include "engine_nocull.h"
#include "engine_patches.h"
#include "engine_shadows.h"
#include "remix_output.h"
#include "remix_ui.h"
#ifdef PROXY_X86_ENGINE
#include "x86_engine.h"
#endif
#ifdef PROXY_GAME_L4D2
#include "slots_shaderapi_l4d2.h"
#elif defined(PROXY_GAME_HL2RTX) || defined(PROXY_GAME_EZ2)
#include "slots_shaderapi_hl2rtx.h"
#elif defined(PROXY_GAME_TF2)
#include "slots_shaderapi_tf2.h"
#elif defined(PROXY_GAME_PORTAL2)
#include "slots_shaderapi_portal2.h"
#endif
#include "vb_origins.h"
#include "vtable_proxy.h"
#ifdef PROXY_GAME_GMOD
#include "workshop_content.h"
#endif

#include <algorithm>
#include <atomic>
#include <tlhelp32.h>

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using CreateInterfaceFn = void *(*)(const char *name, int *returnCode);

HMODULE g_self = nullptr;
HMODULE g_orig = nullptr;
CreateInterfaceFn g_origCreateInterface = nullptr;
FILE *g_log = nullptr;
std::wstring g_dir;
proxy::diag::Workers g_workers;

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
  auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(reinterpret_cast<uint8_t *>(m) + dos->e_lfanew);
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

// Appends a switch to tier0's command line before the original is loaded,
// for the engine to find when it looks it up (ICommandLine::FindParm).
// ICommandLine slots, the same in GMod x64, L4D2, Half-Life 2 RTX and Portal 2
// x86 (L4D2's and Portal 2's engines call FindParm at vtable +0x28, Half-Life
// 2 RTX's shader API too, and its tier0's CheckParm, slot 3): AppendParm 5
// (after CreateCmdLine x2, GetCmdLine, CheckParm, RemoveParm), FindParm 10.
// The accessor's export is game::kCommandLineExport.
void appendSwitch(const char *name) {
  HMODULE tier0 = GetModuleHandleW(L"tier0.dll");
  auto get = tier0 ? reinterpret_cast<void *(*)()>(GetProcAddress(tier0, proxy::game::kCommandLineExport)) : nullptr;
  void *commandLine = get ? get() : nullptr;
  if (!commandLine) {
    log("proxy: no tier0 command line; pass %s yourself", name);
    return;
  }
  void **vt = *static_cast<void ***>(commandLine);
  const auto [begin, end] = codeRangeOf(vt[0]);
  for (int slot : {5, 10})
    if (reinterpret_cast<uintptr_t>(vt[slot]) < begin || reinterpret_cast<uintptr_t>(vt[slot]) >= end) {
      log("proxy: tier0 command line vtable not as expected; pass %s yourself", name);
      return;
    }
  using FindParmFn = int(PROXY_THISCALL *)(void *, const char *);
  using AppendParmFn = void(PROXY_THISCALL *)(void *, const char *, const char *);
  const auto findParm = reinterpret_cast<FindParmFn>(vt[10]);
  if (findParm(commandLine, name)) {
    log("proxy: %s given", name);
  } else {
    reinterpret_cast<AppendParmFn>(vt[5])(commandLine, name, nullptr);
    log("proxy: %s appended to the command line (%s)", name,
        findParm(commandLine, name) ? "found there now" : "NOT found afterwards: pass it yourself");
  }
}

void forceSwitches() {
#if defined(PROXY_GAME_GMOD) || defined(PROXY_GAME_HL2RTX) || defined(PROXY_GAME_TF2)
  // The proxy hooks the original's import of d3d9!Direct3DCreate9 (TF2: its
  // GetProcAddress, d3d_hooks.h). Without -nod3d9ex the shader API asks
  // d3d9.dll for Direct3DCreate9Ex instead (by name, at run time) and creates
  // an IDirect3DDevice9Ex nothing here sees: the game then runs without the
  // proxy's D3D hooks. (Half-Life 2 RTX's and TF2's shader APIs are SDK
  // 2013's in this: CShaderDeviceMgrDx8 asks FindParm for -nod3d9ex, then
  // GetProcAddress(d3d9, "Direct3DCreate9Ex").)
  appendSwitch("-nod3d9ex");
#endif
#if defined(PROXY_GAME_L4D2) || defined(PROXY_GAME_TF2) || defined(PROXY_GAME_EZ2) || defined(PROXY_GAME_PORTAL2)
  // L4D2 (l4d2-rtx's proxy saw the plain device without -nod3d9ex), TF2, EZ2
  // and Portal 2 (its shader API imports only Direct3DCreate9): the proxy
  // replaces a game module, patches engine code and hooks its vtables, which
  // VAC must never see, so the game stays off VAC-secured servers.
  appendSwitch("-insecure");
#endif
}

bool loadOriginal() {
  if (g_orig)
    return true;
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(g_self, path, MAX_PATH);
  g_dir = path;
  g_dir.resize(g_dir.find_last_of(L"\\/"));
  g_log = _wfopen((g_dir + L"\\shaderapi_proxy.log").c_str(), L"w");
  proxy::diag::reinstall();
  if (!proxy::diag::retainHookModule(g_self)) {
    proxy::diag::stage("Failed to retain proxy hook module (error %lu)", GetLastError());
    return false;
  }
  proxy::diag::captureEngineErrors();
  proxy::diag::stage("first CreateInterface: loading shaderapidx9_orig.dll");
  forceSwitches();
  g_orig = LoadLibraryW((g_dir + L"\\shaderapidx9_orig.dll").c_str());
  if (!g_orig) {
    const DWORD error = GetLastError();
    log("proxy: failed to load shaderapidx9_orig.dll (%lu)", error);
    proxy::diag::stage("FAILED to load shaderapidx9_orig.dll (error %lu): was the game's own DLL renamed to it?", error);
    return false;
  }
  g_origCreateInterface = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(g_orig, "CreateInterface"));
  if (!proxy::d3d::install(g_orig))
    log("proxy: neither a d3d9!Direct3DCreate9 nor a GetProcAddress import found; D3D hooks inactive");
  log("proxy: loaded original at %p", static_cast<void *>(g_orig));
  proxy::diag::stage("shaderapidx9_orig.dll loaded at %p, CreateInterface %s", static_cast<void *>(g_orig),
                     g_origCreateInterface ? "found" : "MISSING");
  return g_origCreateInterface != nullptr;
}

void *wrapInterface(const char *name, void *real);

// IAppSystem: Connect 0, Disconnect 1, QueryInterface 2, Init 3, Shutdown 4
// (GMod keeps the classic five, sourcesdk-minimal appframework/iappsystem.h).

// IShaderDeviceMgr slot 2: QueryInterface.
void *PROXY_THISCALL deviceMgrQueryInterface(proxy::Wrapper *self, const char *name) {
  using Fn = void *(PROXY_THISCALL *)(void *, const char *);
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
bool PROXY_THISCALL deviceMgrConnect(proxy::Wrapper *self, CreateInterfaceFn factory) {
  using Fn = bool(PROXY_THISCALL *)(void *, CreateInterfaceFn);
  g_connectFactory = factory;
  log("proxy: IShaderDeviceMgr::Connect(factory %p)", reinterpret_cast<void *>(factory));
  proxy::diag::stage("IShaderDeviceMgr::Connect");
  const bool connected = proxy::original<Fn>(self, 0)(self->real, factory ? connectFactory : nullptr);
  log("proxy: IShaderDeviceMgr::Connect -> %s", connected ? "true" : "false");
  proxy::diag::stage("IShaderDeviceMgr::Connect returned %u", unsigned(connected));
  return connected;
}

#ifdef PROXY_GAME_EZ2
// IShaderDeviceMgr slot 1: void Disconnect() (IAppSystem).
void PROXY_THISCALL deviceMgrDisconnect(proxy::Wrapper *self) {
  using Fn = void(PROXY_THISCALL *)(void *);
  log("proxy: IShaderDeviceMgr::Disconnect");
  proxy::diag::stage("IShaderDeviceMgr::Disconnect");
  proxy::original<Fn>(self, 1)(self->real);
}

#endif

// IShaderDeviceMgr slot 3: InitReturnVal_t Init() (IAppSystem).
int PROXY_THISCALL deviceMgrInit(proxy::Wrapper *self) {
  using Fn = int(PROXY_THISCALL *)(void *);
  log("proxy: IShaderDeviceMgr::Init");
  proxy::diag::stage("IShaderDeviceMgr::Init");
  int rc = proxy::original<Fn>(self, 3)(self->real);
  log("proxy: IShaderDeviceMgr::Init -> %d", rc);
  proxy::diag::stage("IShaderDeviceMgr::Init -> %d", rc);
  return rc;
}

// IShaderDeviceMgr slot 4: void Shutdown() (IAppSystem). The material system
// shuts the shader device down on quit, on the main thread: OpenRemix stops
// here rather than in exit-time teardown.
void PROXY_THISCALL deviceMgrShutdown(proxy::Wrapper *self) {
  using Fn = void(PROXY_THISCALL *)(void *);
  log("proxy: IShaderDeviceMgr::Shutdown");
  proxy::diag::stage("IShaderDeviceMgr::Shutdown (the game is quitting)");
  g_workers.stop();
  proxy::diag::stage("Diagnostic workers joined before engine shutdown");
  proxy::original<Fn>(self, 4)(self->real);
  proxy::remix::shutdown();
}

// IShaderDeviceMgr slot 12 (5 + 7; Portal 2: 15, game::kSlotDeviceMgrSetMode):
// CreateInterfaceFn SetMode(void *hWnd, int nAdapter, const ShaderDeviceInfo_t
// &mode). The material system fetches the shader API interfaces through the
// factory it returns.
CreateInterfaceFn g_setModeFactory = nullptr;
void *setModeFactory(const char *name, int *rc) {
  void *real = g_setModeFactory ? g_setModeFactory(name, rc) : nullptr;
  return wrapInterface(name, real);
}
CreateInterfaceFn PROXY_THISCALL deviceMgrSetMode(proxy::Wrapper *self, void *hwnd, int adapter, const void *mode) {
  using Fn = CreateInterfaceFn(PROXY_THISCALL *)(void *, void *, int, const void *);
  proxy::diag::stage("IShaderDeviceMgr::SetMode(hwnd %p, adapter %d): the engine creates its Direct3D device", hwnd, adapter);
  CreateInterfaceFn factory =
      proxy::original<Fn>(self, proxy::game::kSlotDeviceMgrSetMode)(self->real, hwnd, adapter, mode);
  log("proxy: SetMode(hwnd=%p, adapter=%d) -> factory %p", hwnd, adapter, reinterpret_cast<void *>(factory));
  proxy::diag::stage("SetMode returned factory %p", reinterpret_cast<void *>(factory));
  if (!factory)
    return nullptr;
  g_setModeFactory = factory;
  return setModeFactory;
}

// IShaderUtil (material system) slot 12 (5 + 7; Portal 2: 15,
// game::kSlotRestoreShaderObjects): void RestoreShaderObjects(
// CreateInterfaceFn shaderFactory, int nChangeFlags). The material system
// refreshes its global shader API pointers from this factory.
CreateInterfaceFn g_restoreFactory = nullptr;
void *restoreFactory(const char *name, int *rc) {
  void *real = g_restoreFactory ? g_restoreFactory(name, rc) : nullptr;
  return wrapInterface(name, real);
}
void PROXY_THISCALL shaderUtilRestoreShaderObjects(proxy::Wrapper *self, CreateInterfaceFn factory, int flags) {
  using Fn = void(PROXY_THISCALL *)(void *, CreateInterfaceFn, int);
  log("proxy: IShaderUtil::RestoreShaderObjects(factory %p, flags %d)", reinterpret_cast<void *>(factory), flags);
  if (factory && factory != restoreFactory)
    g_restoreFactory = factory;
  proxy::original<Fn>(self, proxy::game::kSlotRestoreShaderObjects)(self->real, factory ? restoreFactory : nullptr,
                                                                     flags);
}

// IShaderAPI (ShaderApi030) slot 127 (Half-Life 2 RTX 126): void
// SetLight(int lightNum, const LightDesc_t &desc) (copies 88 bytes into its
// light array). SDK 2013 sets the model lights one at a time; L4D2's
// IShaderAPIDX8 slot 88 (Portal 2 92) is void SetLights(int nCount, const
// LightDesc_t *pDesc), an array (game.h).
void PROXY_THISCALL shaderApiSetLight(proxy::Wrapper *self, int index, const void *desc) {
  using Fn = void(PROXY_THISCALL *)(void *, int, const void *);
  proxy::remix::onSetLights(proxy::game::kSetLightsArray ? index : 1, desc);
  proxy::original<Fn>(self, proxy::game::kSlotSetLight)(self->real, index, desc);
}

// IShaderAPI slot 114 (L4D2 76, Half-Life 2 RTX 113, Portal 2 80): void
// Bind(IMaterial *pMaterial): the material of the draws that follow (GMod,
// Half-Life 2 RTX: those buffered for the previous one are flushed inside).
// Hooked only when engine_materials validated the slot.
void PROXY_THISCALL shaderApiBind(proxy::Wrapper *self, void *material) {
  using Fn = void(PROXY_THISCALL *)(void *, void *);
  proxy::original<Fn>(self, proxy::game::kSlotBind)(self->real, material);
  proxy::engine_materials::onBind(material);
}

// IShaderAPI slot 216 (L4D2 181, Half-Life 2 RTX 215, Portal 2 186): void
// SetPIXMarker(unsigned long color, const char *name) (an empty stub in
// release builds). Markers "openremix:..." are ours.
void PROXY_THISCALL shaderApiSetPIXMarker(proxy::Wrapper *self, unsigned long color, const char *name) {
  using Fn = void(PROXY_THISCALL *)(void *, unsigned long, const char *);
  if (proxy::remix::onMarker(name))
    return;
  proxy::original<Fn>(self, proxy::game::kSlotSetPIXMarker)(self->real, color, name);
}

#ifdef PROXY_GAME_GMOD
// IShaderAPI slot 158: ReadPixels of a rect of render target 0. Engine
// screenshots (the camera tool's "jpeg", "screenshot") and Lua captures read
// the back buffer through it, which OpenRemix never draws (and from which the
// shader API reads nothing under the proxy: the files came out black). Reads
// of the back buffer get the runtime's last presented frame instead.
struct ReadRect {
  int x, y, width, height;
};
void PROXY_THISCALL shaderApiReadPixels(proxy::Wrapper *self, ReadRect *src, ReadRect *dst, unsigned char *data,
                                        int format, int stride) {
  using Fn = void(PROXY_THISCALL *)(void *, ReadRect *, ReadRect *, unsigned char *, int, int);
  proxy::original<Fn>(self, proxy::game::kSlotReadPixels)(self->real, src, dst, data, format, stride);
  uint32_t width = 0, height = 0;
  if (!data || !proxy::d3d::backBufferBound() || !proxy::d3d::backBufferSize(width, height))
    return;
  // Each format's bytes as indices into B, G, R, A (4: opaque padding).
  struct Layout {
    int format, bytes;
    int order[4];
  };
  static constexpr Layout kLayouts[] = {
      {0, 4, {2, 1, 0, 3}},  // RGBA8888
      {1, 4, {3, 0, 1, 2}},  // ABGR8888
      {2, 3, {2, 1, 0, 0}},  // RGB888
      {3, 3, {0, 1, 2, 0}},  // BGR888
      {11, 4, {3, 2, 1, 0}}, // ARGB8888
      {12, 4, {0, 1, 2, 3}}, // BGRA8888
      {16, 4, {0, 1, 2, 4}}, // BGRX8888
  };
  const Layout *layout = nullptr;
  for (const auto &l : kLayouts)
    if (l.format == format)
      layout = &l;
  if (!layout)
    return;
  const ReadRect from = src ? *src : ReadRect{0, 0, int(width), int(height)};
  const ReadRect to = dst ? *dst : ReadRect{0, 0, from.width, from.height};
  if (from.width <= 0 || from.height <= 0 || to.width <= 0 || to.height <= 0 || to.x < 0 || to.y < 0)
    return;
  const auto *image = proxy::remix::presentedImage(width, height);
  if (!image)
    return;
  const size_t pitch = stride > 0 ? size_t(stride) : size_t(to.width) * layout->bytes;
  for (int y = 0; y < to.height; ++y) {
    const int sy = std::clamp(from.y + int(int64_t(y) * from.height / to.height), 0, int(height) - 1);
    unsigned char *row = data + size_t(to.y + y) * pitch + size_t(to.x) * layout->bytes;
    for (int x = 0; x < to.width; ++x) {
      const int sx = std::clamp(from.x + int(int64_t(x) * from.width / to.width), 0, int(width) - 1);
      const uint8_t *bgra = image->data() + (size_t(sy) * width + sx) * 4;
      for (int c = 0; c < layout->bytes; ++c)
        row[x * layout->bytes + c] = layout->order[c] == 4 ? 255 : bgra[layout->order[c]];
    }
  }
}
#endif

struct KnownInterface {
  const char *name;
  void (*setup)(proxy::Kind *);
};

const KnownInterface kKnown[] = {
    {"ShaderDeviceMgr001",
     [](proxy::Kind *k) {
       proxy::setHook(k, 0, reinterpret_cast<void *>(&deviceMgrConnect));
#ifdef PROXY_GAME_EZ2
       proxy::setHook(k, 1, reinterpret_cast<void *>(&deviceMgrDisconnect));
#endif
       proxy::setHook(k, 2, reinterpret_cast<void *>(&deviceMgrQueryInterface));
       proxy::setHook(k, 3, reinterpret_cast<void *>(&deviceMgrInit));
       proxy::setHook(k, 4, reinterpret_cast<void *>(&deviceMgrShutdown));
       proxy::setHook(k, proxy::game::kSlotDeviceMgrSetMode, reinterpret_cast<void *>(&deviceMgrSetMode));
     }},
    {proxy::game::kShaderApiInterface,
     [](proxy::Kind *k) {
#ifndef PROXY_GAME_GMOD
       k->slotNames.assign(proxy::slots::kShaderApi, proxy::slots::kShaderApi + std::size(proxy::slots::kShaderApi));
#endif
       proxy::setHook(k, proxy::game::kSlotSetLight, reinterpret_cast<void *>(&shaderApiSetLight));
       proxy::setHook(k, proxy::game::kSlotSetPIXMarker, reinterpret_cast<void *>(&shaderApiSetPIXMarker));
#ifdef PROXY_GAME_GMOD
       proxy::setHook(k, proxy::game::kSlotReadPixels, reinterpret_cast<void *>(&shaderApiReadPixels));
#endif
       if (proxy::engine_materials::install(k->realVtbl))
         proxy::setHook(k, proxy::game::kSlotBind, reinterpret_cast<void *>(&shaderApiBind));
     }},
    {"ShaderDevice001", nullptr},
    {"ShaderShadow010", nullptr},
    {"DebugTextureInfo001", nullptr},
    {proxy::game::kHardwareConfigInterface, nullptr},
    {"VBAllocTracker001",
     [](proxy::Kind *k) {
       if (proxy::game::kHookVBAllocTracker)
         proxy::vb_origins::install(k);
     }},
    // Material system interface handed to the shader API.
    {"VShaderUtil001",
     [](proxy::Kind *k) {
       proxy::setHook(k, proxy::game::kSlotRestoreShaderObjects,
                      reinterpret_cast<void *>(&shaderUtilRestoreShaderObjects));
     }},
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
    if (std::strcmp(name, proxy::game::kShaderApiInterface) == 0)
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
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(reinterpret_cast<uint8_t *>(m) + dos->e_lfanew);
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

// A suspended thread's instruction and stack pointers.
#ifdef _WIN64
uintptr_t instructionPointer(const CONTEXT &c) { return c.Rip; }
uintptr_t stackPointer(const CONTEXT &c) { return c.Rsp; }
#else
uintptr_t instructionPointer(const CONTEXT &c) { return c.Eip; }
uintptr_t stackPointer(const CONTEXT &c) { return c.Esp; }
#endif

// Dev aid: `shaderapi_proxy_profile.request` next to the DLL samples every
// thread for 5 s into shaderapi_proxy_profile.txt as
// "<samples> <thread> <ip> [< [rsp] <code address at the stack top>]".
std::string symbolAt(uintptr_t pc) {
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
  while (GetTickCount64() < end && !g_workers.stopping()) {
    for (auto [id, thread] : threads) {
      CONTEXT context = {};
      context.ContextFlags = CONTEXT_CONTROL;
      if (SuspendThread(thread) == DWORD(-1))
        continue;
      // While suspended: no allocation, no locks (the thread may hold the
      // heap or loader lock). Instruction pointer and the qword at RSP: the
      // return address when the thread is in a leaf function (memcpy).
      uintptr_t pc = 0, top[8] = {};
      if (GetThreadContext(thread, &context)) {
        pc = instructionPointer(context);
        const uintptr_t sp = stackPointer(context);
        MEMORY_BASIC_INFORMATION stack = {};
        if (VirtualQuery(reinterpret_cast<void *>(sp), &stack, sizeof(stack)) && stack.State == MEM_COMMIT &&
            sp + sizeof(top) <= uintptr_t(stack.BaseAddress) + stack.RegionSize)
          std::memcpy(top, reinterpret_cast<void *>(sp), sizeof(top));
      }
      ResumeThread(thread);
      if (!pc)
        continue;
      // Caller: the first code address in another module among the top stack
      // slots (leaf functions that saved registers keep it a few slots up).
      std::string chain = symbolAt(pc);
      const auto own = codeRangeOf(reinterpret_cast<void *>(pc));
      for (uintptr_t v : top) {
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

#ifdef PROXY_GAME_GMOD
// Dev aid: `shaderapi_proxy_close.request` closes the game's windows
// (WM_CLOSE, as the window's close button; GMod blocks `quit` from Lua).
BOOL CALLBACK closeWindow(HWND window, LPARAM) {
  DWORD process = 0;
  GetWindowThreadProcessId(window, &process);
  if (process == GetCurrentProcessId() && IsWindowVisible(window))
    PostMessageW(window, WM_CLOSE, 0, 0);
  return TRUE;
}
#endif

DWORD WINAPI profilerThread(void *) {
  const std::wstring request = g_dir + L"\\shaderapi_proxy_profile.request";
#ifdef PROXY_GAME_GMOD
  const std::wstring close = g_dir + L"\\shaderapi_proxy_close.request";
#endif
  while (!g_workers.wait(250)) {
#ifdef PROXY_GAME_GMOD
    if (GetFileAttributesW(close.c_str()) != INVALID_FILE_ATTRIBUTES) {
      DeleteFileW(close.c_str());
      EnumWindows(closeWindow, 0);
    }
#endif
    if (GetFileAttributesW(request.c_str()) == INVALID_FILE_ATTRIBUTES)
      continue;
    DeleteFileW(request.c_str());
    sampleThreads();
  }
  return 0;
}

// The engine asks every loose search path for a file before its packages
// (.vpk), each time it wants it: an animation block of a ragdoll or an NPC is
// asked for hundreds of times in three or more directories that do not have
// it. gm_construct, a scripted minute of play: 19,000 failed opens, 456 ms,
// most of them in the few frames that load a model. A read-only open that
// found no file is remembered for kMissSeconds and fails at once until then
// (filesystem_stdio.dll's CreateFileA/W; OPENREMIX_PROXY_FILE_MISSES=0 leaves
// them alone). Not under garrysmod\data, which scripts and tools write at any
// time; an open that may create the file forgets it.
//
// Dev aid (with OPENREMIX_PROXY_STALLS): the opens on the presenting thread
// to shaderapi_proxy_opens.txt beside the stalls: per path, the calls, how
// many failed, how many of those were answered from memory, and the time.
struct FileOpens {
  unsigned calls = 0, failed = 0, remembered = 0;
  double ms = 0;
};
std::mutex g_openLock;
std::map<std::string, FileOpens> g_opens;
std::atomic_bool g_noteOpens{false};
#ifdef PROXY_GAME_GMOD
using CreateFileAFn = HANDLE(WINAPI *)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileWFn = HANDLE(WINAPI *)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
CreateFileAFn g_createFileA = nullptr;
CreateFileWFn g_createFileW = nullptr;

constexpr ULONGLONG kMissSeconds = 3;
struct FileMiss {
  ULONGLONG tick = 0;
  DWORD error = 0;
};
std::mutex g_missLock;
std::unordered_map<std::string, FileMiss> g_misses;

void noteOpen(const std::string &path, bool failed, bool remembered, const LARGE_INTEGER &began) {
  LARGE_INTEGER now, frequency;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&frequency);
  unsigned long thread = 0;
  double frame = 0;
  if (!proxy::remix::frameInProgress(thread, frame) || thread != GetCurrentThreadId())
    return;
  std::lock_guard<std::mutex> lock(g_openLock);
  auto &o = g_opens[path];
  ++o.calls;
  o.failed += failed;
  o.remembered += remembered;
  o.ms += double(now.QuadPart - began.QuadPart) * 1000 / double(frequency.QuadPart);
}

// `path` lowercased with backslashes; false for one that is never remembered.
bool missKey(std::string &path) {
  for (char &c : path) {
    if (static_cast<unsigned char>(c) >= 128)
      return false;
    c = c == '/' ? '\\' : char(std::tolower(static_cast<unsigned char>(c)));
  }
  return !path.empty() && path.find("\\garrysmod\\data\\") == std::string::npos;
}

template <typename Open>
HANDLE openFile(std::string path, DWORD access, DWORD disposition, Open &&open) {
  static const bool remember = [] {
    char v[8] = {};
    return !(GetEnvironmentVariableA("OPENREMIX_PROXY_FILE_MISSES", v, sizeof(v)) && v[0] == '0');
  }();
  LARGE_INTEGER began;
  if (g_noteOpens)
    QueryPerformanceCounter(&began);
  const bool keyed = remember && missKey(path);
  const bool reading = disposition == OPEN_EXISTING && !(access & ~(GENERIC_READ | FILE_GENERIC_READ));
  if (keyed) {
    std::lock_guard<std::mutex> lock(g_missLock);
    auto found = g_misses.find(path);
    if (found != g_misses.end()) {
      if (reading && GetTickCount64() - found->second.tick < kMissSeconds * 1000) {
        const DWORD error = found->second.error;
        if (g_noteOpens)
          noteOpen(path, true, true, began);
        SetLastError(error);
        return INVALID_HANDLE_VALUE;
      }
      g_misses.erase(found);
    }
  }
  const HANDLE file = open();
  const DWORD error = GetLastError();
  if (keyed && reading && file == INVALID_HANDLE_VALUE &&
      (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)) {
    std::lock_guard<std::mutex> lock(g_missLock);
    if (g_misses.size() > 200000)
      g_misses.clear();
    g_misses[path] = {GetTickCount64(), error};
  }
  if (g_noteOpens)
    noteOpen(path, file == INVALID_HANDLE_VALUE, false, began);
  SetLastError(error);
  return file;
}

// The video options' "Advanced..." button becomes "OpenRemix Settings...",
// which opens OpenRemix's panel: the settings the Advanced page held (model,
// texture and shader detail, water, shadows, antialiasing, filtering, HDR...)
// are given on the command line (README) and mat_picmip by the proxy
// (engine_hooks.cpp). GameUI reads the page's layout from
// resource/optionssubvideo.res; an open of it gets a copy beside the proxy, of
// the same size (the file system may know it from a stat), where the button
// has the new label and the command "openremix_settings". COptionsSubVideo
// does not handle commands itself (its Advanced button posts a message of its
// own), so its OnCommand (vgui::Panel slot 95 in GMod's GameUI.dll) is
// patched to answer that one.
// OPENREMIX_PROXY_ADVANCED_VIDEO=1 leaves the page as it is.
bool videoOptionsLayout(const std::string &key) {
  static const bool replace = [] {
    char v[8] = {};
    return !(GetEnvironmentVariableA("OPENREMIX_PROXY_ADVANCED_VIDEO", v, sizeof(v)) && v[0] == '1');
  }();
  static const std::string suffix = "\\resource\\optionssubvideo.res";
  return replace && key.size() > suffix.size() && key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0;
}

using OnCommandFn = void(PROXY_THISCALL *)(void *, const char *);
OnCommandFn g_videoOnCommand = nullptr;
void PROXY_THISCALL videoOnCommand(void *self, const char *command) {
  if (command && _stricmp(command, "openremix_settings") == 0)
    proxy::remix_ui::open();
  else
    g_videoOnCommand(self, command);
}

// COptionsSubVideo's vtable in GameUI.dll (RVA 0x12db68 in the build this was
// written for), checked by its RTTI name; its OnCommand slot is patched.
bool hookVideoOnCommand() {
  constexpr uintptr_t kVtable = 0x12db68;
  constexpr unsigned kOnCommand = 95;
  HMODULE module = GetModuleHandleW(L"GameUI.dll");
  if (!module)
    return false;
  const auto base = reinterpret_cast<uintptr_t>(module);
  const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + reinterpret_cast<const IMAGE_DOS_HEADER *>(module)->e_lfanew);
  const uintptr_t size = nt->OptionalHeader.SizeOfImage;
  auto inside = [&](uintptr_t address, size_t bytes) { return address >= base && address - base + bytes <= size; };
  auto **vtable = reinterpret_cast<void **>(base + kVtable);
  if (!inside(reinterpret_cast<uintptr_t>(vtable - 1), (kOnCommand + 2) * sizeof(void *)))
    return false;
  // The complete object locator before the vtable: its type descriptor (RVA
  // at +12) holds the decorated name at +16.
  const auto locator = reinterpret_cast<uintptr_t>(vtable[-1]);
  if (!inside(locator, 24))
    return false;
  const uintptr_t type = base + *reinterpret_cast<const uint32_t *>(locator + 12);
  static const char kName[] = ".?AVCOptionsSubVideo@@";
  if (!inside(type + 16, sizeof(kName)) || std::memcmp(reinterpret_cast<const char *>(type + 16), kName, sizeof(kName)) != 0 ||
      !inside(reinterpret_cast<uintptr_t>(vtable[kOnCommand]), 1))
    return false;
  DWORD old;
  if (!VirtualProtect(&vtable[kOnCommand], sizeof(void *), PAGE_READWRITE, &old))
    return false;
  g_videoOnCommand = reinterpret_cast<OnCommandFn>(vtable[kOnCommand]);
  vtable[kOnCommand] = reinterpret_cast<void *>(&videoOnCommand);
  VirtualProtect(&vtable[kOnCommand], sizeof(void *), old, &old);
  return true;
}

// The layout's text with the settings button, the size of `text`; empty when
// it cannot be made.
std::string settingsLayout(std::string text) {
  // The key and value of a "key" "value" line.
  auto field = [](const std::string &line, std::string &key, std::string &value) {
    size_t q[4], at = 0;
    for (size_t &i : q) {
      i = line.find('"', at);
      if (i == std::string::npos)
        return false;
      at = i + 1;
    }
    key = line.substr(q[0] + 1, q[1] - q[0] - 1);
    value = line.substr(q[2] + 1, q[3] - q[2] - 1);
    return true;
  };
  const size_t original = text.size();
  const size_t name = text.find("\"AdvancedButton\"");
  const size_t open = name == std::string::npos ? name : text.find('{', name);
  const size_t close = open == std::string::npos ? open : text.find('}', open);
  if (close == std::string::npos)
    return {};
  // The button's label; its command, before the block's closing brace.
  const size_t label = text.find("\"labelText\"", open);
  if (label == std::string::npos || label > close)
    return {};
  const size_t valueStart = text.find('"', label + 11), valueEnd = valueStart == std::string::npos ? valueStart : text.find('"', valueStart + 1);
  if (valueEnd == std::string::npos || valueEnd > close)
    return {};
  const std::string eol = text.find("\r\n", open) < close ? "\r\n" : "\n";
  const size_t lineStart = text.rfind('\n', label) + 1;
  const std::string indent = text.substr(lineStart, label - lineStart);
  text.replace(valueStart + 1, valueEnd - valueStart - 1, "OpenRemix Settings...");
  const size_t brace = text.find('}', open);
  text.insert(text.rfind('\n', brace) + 1, indent + "\"command\"\t\t\"openremix_settings\"" + eol);
  // Room: lines that give a control's default value go.
  static const char *const defaults[][2] = {{"autoResize", "0"}, {"pinCorner", "0"}, {"dulltext", "0"},
                                            {"brighttext", "0"}, {"wrap", "0"},       {"Default", "0"},
                                            {"unicode", "0"},    {"textHidden", "0"}, {"editable", "0"},
                                            {"NumericInputOnly", "0"}, {"maxchars", "-1"}, {"selected", "0"}};
  for (size_t at = 0; text.size() > original && at < text.size();) {
    const size_t next = text.find('\n', at);
    const size_t end = next == std::string::npos ? text.size() : next + 1;
    std::string key, value;
    bool drop = false;
    if (field(text.substr(at, end - at), key, value))
      for (const auto &d : defaults)
        drop |= key == d[0] && value == d[1];
    if (drop)
      text.erase(at, end - at);
    else
      at = end;
  }
  if (text.size() > original)
    return {};
  text.insert(text.rfind('}'), original - text.size(), ' ');
  return text;
}

// The copy of `original` with the settings button; empty when it cannot be
// made (the original is then opened). `original` is a synchronous handle: the
// file system's own may be overlapped.
std::wstring videoOptionsCopy(HANDLE original) {
  if (original == INVALID_HANDLE_VALUE)
    return {};
  std::string text;
  char buffer[4096];
  DWORD read = 0;
  while (ReadFile(original, buffer, sizeof(buffer), &read, nullptr) && read)
    text.append(buffer, read);
  CloseHandle(original);
  // GameUI is loaded by now; without the hook the button keeps its place.
  if (!g_videoOnCommand && !hookVideoOnCommand())
    return {};
  text = settingsLayout(std::move(text));
  if (text.empty())
    return {};
  const std::wstring path = g_dir + L"\\shaderapi_proxy_optionssubvideo.res";
  FILE *f = _wfopen(path.c_str(), L"wb");
  if (!f)
    return {};
  const bool written = fwrite(text.data(), 1, text.size(), f) == text.size();
  fclose(f);
  return written ? path : std::wstring();
}

// An open of the video options' layout, answered with the copy (else as
// asked). `plain` opens the file for synchronous reading.
template <typename Open, typename Plain>
HANDLE openVideoOptions(Open &&open, Plain &&plain, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                        DWORD disposition, DWORD flags, HANDLE model) {
  const std::wstring copy = videoOptionsCopy(plain());
  if (copy.empty())
    return open();
  log("video options: the Advanced button opens OpenRemix's settings (OPENREMIX_PROXY_ADVANCED_VIDEO=1 keeps it)\n");
  return g_createFileW(copy.c_str(), access, share, security, disposition, flags, model);
}

HANDLE WINAPI createFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security, DWORD disposition,
                          DWORD flags, HANDLE model) {
  if (std::string key = name ? name : ""; missKey(key) && videoOptionsLayout(key))
    return openVideoOptions(
        [&] { return g_createFileA(name, access, share, security, disposition, flags, model); },
        [&] { return g_createFileA(name, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr); },
        access, share, security, disposition, flags, model);
  return openFile(name ? name : "", access, disposition,
                  [&] { return g_createFileA(name, access, share, security, disposition, flags, model); });
}

HANDLE WINAPI createFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security, DWORD disposition,
                          DWORD flags, HANDLE model) {
  std::string narrow;
  for (const wchar_t *c = name; c && *c; ++c)
    narrow += *c < 128 ? char(*c) : char(0xff); // not ASCII: never remembered
  if (std::string key = narrow; missKey(key) && videoOptionsLayout(key))
    return openVideoOptions(
        [&] { return g_createFileW(name, access, share, security, disposition, flags, model); },
        [&] { return g_createFileW(name, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr); },
        access, share, security, disposition, flags, model);
  return openFile(std::move(narrow), access, disposition,
                  [&] { return g_createFileW(name, access, share, security, disposition, flags, model); });
}

void hookFileOpens() {
  HMODULE fs = GetModuleHandleW(L"filesystem_stdio.dll");
  if (!fs)
    return;
  g_createFileA = reinterpret_cast<CreateFileAFn>(
      proxy::d3d::patchModuleImport(fs, "KERNEL32.dll", "CreateFileA", reinterpret_cast<void *>(&createFileA)));
  g_createFileW = reinterpret_cast<CreateFileWFn>(
      proxy::d3d::patchModuleImport(fs, "KERNEL32.dll", "CreateFileW", reinterpret_cast<void *>(&createFileW)));
  log("file opens: CreateFileA %p CreateFileW %p\n", reinterpret_cast<void *>(g_createFileA),
      reinterpret_cast<void *>(g_createFileW));
}
#else
// L4D2, Half-Life 2 RTX: neither the remembered misses nor the video options'
// layout (GMod's GameUI build, its search paths); the opens are not counted.
void hookFileOpens() {}
#endif

void writeFileOpens(bool reset) {
  std::lock_guard<std::mutex> lock(g_openLock);
  if (reset)
    g_opens.clear();
  FILE *f = _wfopen((g_dir + L"\\shaderapi_proxy_opens.txt").c_str(), L"w");
  if (!f)
    return;
  FileOpens all;
  std::vector<std::pair<double, const std::pair<const std::string, FileOpens> *>> sorted;
  for (const auto &entry : g_opens) {
    all.calls += entry.second.calls;
    all.failed += entry.second.failed;
    all.remembered += entry.second.remembered;
    all.ms += entry.second.ms;
    sorted.emplace_back(entry.second.ms, &entry);
  }
  std::sort(sorted.rbegin(), sorted.rend());
  fprintf(f, "%u opens on the presenting thread, %u failed (%u of them remembered), %.1f ms, %zu paths\n", all.calls,
          all.failed, all.remembered, all.ms, g_opens.size());
  for (const auto &[ms, entry] : sorted)
    fprintf(f, "%8.2f ms %4u calls %4u failed %4u remembered %s\n", ms, entry->second.calls, entry->second.failed,
            entry->second.remembered, entry->first.c_str());
  fclose(f);
}

// Dev aid (OPENREMIX_PROXY_STALLS=<ms>): what the presenting thread runs in
// the frames that take longer than <ms>. It is sampled only while its frame
// has already run that long, so shaderapi_proxy_stalls.txt (rewritten every
// 2 s) holds the stalls alone: "<samples> <ip> < <code addresses up its
// stack>" (a scan of the stack's top, not an unwind: stale addresses may show).
// It counts from launch, or from the last `shaderapi_proxy_stalls.reset` (a
// file next to the DLL, removed when seen: dev/bench.sh makes one as a run
// begins, so the file is that run's).
DWORD WINAPI stallThread(void *parameter) {
  const double limit = double(reinterpret_cast<uintptr_t>(parameter));
  std::map<std::string, unsigned> counts;
  HANDLE thread = nullptr;
  DWORD opened = 0;
  static uintptr_t stack[384];
  ULONGLONG written = GetTickCount64();
  unsigned samples = 0;
  g_noteOpens = true;

  while (!g_workers.wait(1)) {
    unsigned long id = 0;
    double ms = 0;
    if (proxy::remix::frameInProgress(id, ms) && ms > limit && ms < 1000) {
      if (id != opened) {
        if (thread)
          CloseHandle(thread);
        thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, id);
        opened = id;
      }
      CONTEXT context = {};
      context.ContextFlags = CONTEXT_CONTROL;
      uintptr_t pc = 0;
      size_t n = 0;
      if (thread && SuspendThread(thread) != DWORD(-1)) {
        // While suspended: no allocation, no locks.
        if (GetThreadContext(thread, &context)) {
          pc = instructionPointer(context);
          const uintptr_t sp = stackPointer(context);
          MEMORY_BASIC_INFORMATION region = {};
          if (VirtualQuery(reinterpret_cast<void *>(sp), &region, sizeof(region)) && region.State == MEM_COMMIT) {
            const uintptr_t end = uintptr_t(region.BaseAddress) + region.RegionSize;
            n = std::min<size_t>(std::size(stack), (end - sp) / sizeof(uintptr_t));
            std::memcpy(stack, reinterpret_cast<void *>(sp), n * sizeof(uintptr_t));
          }
        }
        ResumeThread(thread);
      }
      if (pc) {
        std::string chain = symbolAt(pc);
        unsigned found = 0;
        for (size_t i = 0; i < n && found < 14; ++i) {
          const auto range = codeRangeOf(reinterpret_cast<void *>(stack[i]));
          if (range.first && stack[i] >= range.first && stack[i] < range.second) {
            chain += " < " + symbolAt(stack[i]);
            ++found;
          }
        }
        ++counts[chain];
        ++samples;
      }
    }
    if (GetTickCount64() - written > 2000) {
      written = GetTickCount64();
      const bool reset = DeleteFileW((g_dir + L"\\shaderapi_proxy_stalls.reset").c_str());
      if (reset) {
        counts.clear();
        samples = 0;
      }
      writeFileOpens(reset);
      std::vector<std::pair<unsigned, std::string>> sorted;
      for (const auto &[chain, count] : counts)
        sorted.emplace_back(count, chain);
      std::sort(sorted.rbegin(), sorted.rend());
      if (FILE *f = _wfopen((g_dir + L"\\shaderapi_proxy_stalls.txt").c_str(), L"w")) {
        fprintf(f, "%u samples in frames over %.0f ms\n", samples, limit);
        for (const auto &[count, chain] : sorted)
          fprintf(f, "%u %s\n", count, chain.c_str());
        fclose(f);
      }
    }
  }
  if (thread) CloseHandle(thread);
  g_noteOpens = false;
  return 0;
}

// Per-slot call counts to shaderapi_proxy_slots.txt every 10 s.
DWORD WINAPI statisticsThread(void *) {
  for (int tick = 0; !g_workers.wait(10000); ++tick) {
    if (tick == 3)
      scanForRealPointers();
    FILE *f = _wfopen((g_dir + L"\\shaderapi_proxy_slots.txt").c_str(), L"w");
    if (f) {
      proxy::remix::noteStatisticsDump(true);
      proxy::d3d::dumpStatistics(f);
      proxy::engine_patches::dumpStatistics(f);
      proxy::engine_nocull::dumpStatistics(f);
      proxy::engine_lights::dumpStatistics(f);
      proxy::engine_shadows::dumpStatistics(f);
      proxy::engine_mirrors::dumpStatistics(f);
#ifdef PROXY_X86_ENGINE
      proxy::x86::dumpEngineStatistics(f);
#endif
      proxy::engine_materials::dumpStatistics(f);
      proxy::dumpStatistics(f);
      fclose(f);
      proxy::remix::noteStatisticsDump(false);
    }
  }
  return 0;
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
#ifdef PROXY_GAME_GMOD
    // Workshop content packages (workshop_content.h): with a runtime this
    // session, their add-on folders now, ahead of GMod mounting
    // garrysmod/addons, and their Remix mods on a thread that startRemix waits
    // for; without one, nothing is placed and their add-ons that need it are
    // hidden.
    proxy::workshop::begin(proxy::remix::enabled() &&
                               GetFileAttributesW((g_dir + proxy::game::kRuntimePath).c_str()) != INVALID_FILE_ATTRIBUTES,
                           proxy::remix::modsDirectory());
#endif
    hookFileOpens();
    if (!g_workers.start(statisticsThread) || !g_workers.start(profilerThread))
      log("proxy: diagnostic worker creation failed (%lu)", GetLastError());
    char stalls[16] = {};
    if (GetEnvironmentVariableA("OPENREMIX_PROXY_STALLS", stalls, sizeof(stalls)) && std::atoi(stalls) > 0)
      if (!g_workers.start(stallThread, reinterpret_cast<void *>(uintptr_t(std::atoi(stalls)))))
        log("proxy: stall sampler creation failed (%lu)", GetLastError());
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
    proxy::diag::install(instance);
    proxy::diag::stage("proxy loaded");
    preferCachedDxvkBuffers();
  } else if (reason == DLL_PROCESS_DETACH) {
    proxy::diag::uninstall();
  }
#ifdef PROXY_GAME_EZ2
  else if (reason == DLL_PROCESS_DETACH) {
    proxy::diag::uninstall();
  }
#endif
  return TRUE;
}
