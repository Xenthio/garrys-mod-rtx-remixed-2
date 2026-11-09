#include "engine_mirrors.h"
#include "bsp_world.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

// client.dll (GMod x64): CViewRender::ViewDrawScene asks
// IsReflectiveGlassInView (0x1db050) whether a reflective glass entity faces
// the camera inside the frustum, and if so draws a CReflectiveGlassView (the
// scene mirrored through the glass's plane, into _rt_WaterReflection) and a
// CRefractiveGlassView (the scene behind it, into _rt_WaterRefraction), each
// through AddViewToScene (the render executor's slot 0, which draws it then
// and there). The reflection stays: the local player's model is drawn only in
// it, and the proxy takes it from there for the mirror (a reflection-only
// instance). The refraction's AddViewToScene call becomes a 5-byte nop: that
// view is set up and released as before, never drawn.
namespace proxy::engine_mirrors {
namespace {

constexpr uintptr_t kCall = 0x30db4d;
// mov [r12+0x143],al; mov rax,[rcx]; call [rax]; mov rax,[...]
constexpr uint8_t kOriginal[] = {0x41, 0x88, 0x84, 0x24, 0x43, 0x01, 0x00, 0x00, 0x48,
                                 0x8B, 0x01, 0xFF, 0x10, 0x48, 0x8B, 0x05, 0x47, 0x36};
constexpr size_t kCallOffset = 8; // the call's first byte in kOriginal
constexpr uint8_t kPatched[5] = {0x0F, 0x1F, 0x44, 0x00, 0x00};
struct Expected {
  uintptr_t rva;
  const char *bytes;
};
constexpr Expected kExpected[] = {
    // lea rax,[CRefractiveGlassView vtable]; mov [r12],rax (the view the call draws)
    {0x30d9db, "\x48\x8D\x05\x16\x53\x3E\x00\x49\x89\x04\x24"},
    // the reflection's AddViewToScene (kept); new CRefractiveGlassView
    {0x30d96f, "\x48\x8B\x01\xFF\x10\xB9\x48\x01\x00\x00"},
};

const char *g_state = "not installed";

} // namespace

void install() {
  static bool done = false;
  if (done)
    return;
  if (!bsp::mirrorsOn()) {
    g_state = "off (OPENREMIX_PROXY_MIRRORS=0: the engine draws the glass)";
    done = true;
    return;
  }
  char v[8] = {};
  if (GetEnvironmentVariableA("OPENREMIX_PROXY_GLASS_VIEWS", v, sizeof(v)) && v[0] == '1') {
    g_state = "off (OPENREMIX_PROXY_GLASS_VIEWS=1)";
    done = true;
    return;
  }
  auto *base = reinterpret_cast<uint8_t *>(GetModuleHandleA("client.dll"));
  if (!base)
    return; // not loaded yet
  done = true;
  const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
  const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
  const uintptr_t size = nt->OptionalHeader.SizeOfImage;
  uint8_t *const site = base + kCall - kCallOffset;
  if (kCall - kCallOffset + sizeof(kOriginal) > size || std::memcmp(site, kOriginal, sizeof(kOriginal)) != 0) {
    g_state = "client.dll differs (the glass views are kept)";
    return;
  }
  for (const Expected &e : kExpected) {
    const size_t n = std::strlen(e.bytes);
    if (e.rva + n > size || std::memcmp(base + e.rva, e.bytes, n) != 0) {
      g_state = "client.dll differs (the glass views are kept)";
      return;
    }
  }
  DWORD old;
  if (!VirtualProtect(base + kCall, sizeof(kPatched), PAGE_EXECUTE_READWRITE, &old)) {
    g_state = "cannot make the code writable";
    return;
  }
  std::memcpy(base + kCall, kPatched, sizeof(kPatched));
  VirtualProtect(base + kCall, sizeof(kPatched), old, &old);
  FlushInstructionCache(GetCurrentProcess(), base + kCall, sizeof(kPatched));
  g_state = "refraction view skipped (path-traced mirrors; the reflection view draws the player's model)";
}

void dumpStatistics(FILE *out) { fprintf(out, "== Engine reflective glass views: %s\n", g_state); }

} // namespace proxy::engine_mirrors
