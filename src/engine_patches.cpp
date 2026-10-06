#include "engine_patches.h"
#include "bsp_world.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

// engine_hooks.cpp: IVRenderView slot 13 as the engine had it (the thunk the
// call chain is decoded from).
extern "C" void *openremix_drawWorldListsOriginal;

namespace proxy::engine_patches {
namespace {

// The loaded engine.dll: everything read or patched is bounds-checked
// against it, so a different build fails validation instead of faulting.
struct Image {
  uint8_t *base = nullptr, *end = nullptr;
  uint8_t *text = nullptr, *textEnd = nullptr;
  struct Range {
    uint8_t *begin, *end;
  };
  std::vector<Range> readOnly; // .rdata and the like: where vtables live
  bool inside(const void *p, size_t n) const {
    auto b = static_cast<const uint8_t *>(p);
    return b >= base && n <= size_t(end - base) && b <= end - n;
  }
  bool code(const void *p, size_t n) const {
    auto b = static_cast<const uint8_t *>(p);
    return b >= text && n <= size_t(textEnd - text) && b <= textEnd - n;
  }
  bool constant(const void *p, size_t n) const {
    auto b = static_cast<const uint8_t *>(p);
    for (const Range &r : readOnly)
      if (b >= r.begin && n <= size_t(r.end - r.begin) && b <= r.end - n)
        return true;
    return false;
  }
} g_image;

bool openImage() {
  auto base = reinterpret_cast<uint8_t *>(GetModuleHandleA("engine.dll"));
  if (!base)
    return false;
  auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    return false;
  auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
    return false;
  g_image.base = base;
  g_image.end = base + nt->OptionalHeader.SizeOfImage;
  const IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
    uint8_t *begin = base + section->VirtualAddress;
    if (!g_image.inside(begin, section->Misc.VirtualSize))
      return false;
    const DWORD flags = section->Characteristics;
    if (flags & IMAGE_SCN_MEM_DISCARDABLE)
      continue;
    if (flags & IMAGE_SCN_MEM_EXECUTE) {
      if (!g_image.text) {
        g_image.text = begin;
        g_image.textEnd = begin + section->Misc.VirtualSize;
      }
    } else if ((flags & IMAGE_SCN_MEM_READ) && !(flags & IMAGE_SCN_MEM_WRITE)) {
      g_image.readOnly.push_back({begin, begin + section->Misc.VirtualSize});
    }
  }
  return g_image.text != nullptr;
}

// "48 8B ? C3": hex bytes, ? matches anything.
std::vector<int> pattern(const char *text) {
  std::vector<int> out;
  for (const char *p = text; *p;) {
    if (*p == ' ') {
      ++p;
    } else if (*p == '?') {
      out.push_back(-1);
      ++p;
    } else {
      out.push_back(int(std::strtoul(p, nullptr, 16)));
      p += 2;
    }
  }
  return out;
}

bool matches(const uint8_t *at, const std::vector<int> &pat) {
  for (size_t i = 0; i < pat.size(); ++i)
    if (pat[i] >= 0 && at[i] != uint8_t(pat[i]))
      return false;
  return true;
}

bool codeMatches(const void *at, const char *text) {
  const std::vector<int> pat = pattern(text);
  return g_image.code(at, pat.size()) && matches(static_cast<const uint8_t *>(at), pat);
}

// Occurrences in the code section (the pattern starts with a fixed byte).
size_t scanCode(const char *text, uint8_t *&first) {
  const std::vector<int> pat = pattern(text);
  size_t count = 0;
  first = nullptr;
  if (pat.empty() || pat[0] < 0 || size_t(g_image.textEnd - g_image.text) < pat.size())
    return 0;
  const uint8_t *last = g_image.textEnd - pat.size();
  for (const uint8_t *p = g_image.text; p <= last; ++p) {
    p = static_cast<const uint8_t *>(std::memchr(p, pat[0], size_t(last - p) + 1));
    if (!p)
      break;
    if (matches(p, pat) && count++ == 0)
      first = const_cast<uint8_t *>(p);
  }
  return count;
}

// Pointers to `target` in the read-only data (vtable slots).
size_t scanPointers(const void *target, void **&first) {
  size_t count = 0;
  first = nullptr;
  for (const Image::Range &r : g_image.readOnly) {
    auto p = reinterpret_cast<void **>((reinterpret_cast<uintptr_t>(r.begin) + 7) & ~uintptr_t(7));
    for (; reinterpret_cast<uint8_t *>(p + 1) <= r.end; ++p)
      if (*p == target && count++ == 0)
        first = p;
  }
  return count;
}

uint8_t *relative(uint8_t *instructionEnd) {
  int32_t displacement;
  std::memcpy(&displacement, instructionEnd - 4, 4);
  return instructionEnd + displacement;
}

constexpr char kDrawChainsEntry[] = "40 53 48 83 EC 60 48 89 6C 24 70 41 8B E8";
// Not unique in the file on its own: only accepted as a call target of
// Shader_WorldEnd.
constexpr char kZFillEntry[] = "89 54 24 10 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24";
constexpr char kDispBatchSite[] = "48 8B 0E 44 8B 46 68 48 8B 56 48 48 8B 01 FF 50 60 E9";
constexpr size_t kDispBatchCall = 14;
constexpr char kDrawStaticPropsEntry[] = "48 8B C4 48 83 EC 48 48 89 58 08 48 8B D9";
constexpr char kRenderOverlaysEntry[] = "4C 89 4C 24 20 48 89 54 24 10 48 89 4C 24 08 55";
constexpr unsigned kDrawStaticPropsSlot = 13; // IStaticPropMgrClient
constexpr unsigned kRenderOverlaysSlot = 7;   // IOverlayMgr
// Shader_WorldEnd's extent (0x510 bytes in the build this was written for).
constexpr size_t kWorldEndSpan = 0x520;

// IVRenderView::DrawWorldLists -> CRender::DrawWorldLists -> R_DrawWorldLists
// -> Shader_WorldEnd, each step validated by its instruction bytes.
uint8_t *findWorldEnd() {
  auto thunk = static_cast<uint8_t *>(openremix_drawWorldListsOriginal);
  // mov rcx,[g_EngineRenderer]; mov r9,[rcx]; jmp [r9+0x40]
  if (!codeMatches(thunk, "48 8B 0D ? ? ? ? 4C 8B 09 49 FF 61 40"))
    return nullptr;
  uint8_t *global = relative(thunk + 7);
  if (!g_image.inside(global, 8))
    return nullptr;
  void ***renderer = *reinterpret_cast<void ****>(global);
  if (!g_image.inside(renderer, 8) || !g_image.constant(*renderer, 9 * sizeof(void *)))
    return nullptr;
  auto method = static_cast<uint8_t *>((*renderer)[8]);
  // mov r9,[rsp+0x28]; mov rcx,rdx; mov edx,r8d; movaps xmm2,xmm3; jmp rel32
  if (!codeMatches(method, "4C 8B 4C 24 28 48 8B CA 41 8B D0 0F 28 D3 E9 ? ? ? ?"))
    return nullptr;
  uint8_t *drawWorldLists = relative(method + 19);
  // cmp byte [a],0; jne ret; cmp byte [b],0; je Shader_WorldEnd; ret
  if (!codeMatches(drawWorldLists, "80 3D ? ? ? ? 00 75 0D 80 3D ? ? ? ? 00 0F 84 ? ? ? ? C3"))
    return nullptr;
  uint8_t *worldEnd = relative(drawWorldLists + 22);
  return g_image.code(worldEnd, kWorldEndSpan) ? worldEnd : nullptr;
}

// The distinct direct-call targets in Shader_WorldEnd that start with `entry`.
size_t calledFromWorldEnd(uint8_t *worldEnd, const char *entry, uint8_t *&target) {
  size_t count = 0;
  target = nullptr;
  for (size_t i = 0; i + 5 <= kWorldEndSpan; ++i) {
    if (worldEnd[i] != 0xE8)
      continue;
    uint8_t *callee = relative(worldEnd + i + 5);
    if (callee == target || !codeMatches(callee, entry))
      continue;
    target = callee;
    ++count;
  }
  return count;
}

// A few bytes of engine code flipped around DrawWorldLists (main thread).
struct CodePatch {
  const char *name;
  uint8_t original[3], patched[3];
  size_t size;
  uint8_t *at = nullptr;
  bool usable = false, applied = false;
  std::atomic<const char *> state{"not located"};
};
CodePatch g_drawChains{"DrawChains", {0x40}, {0xC3}, 1};          // rex push rbx -> ret
CodePatch g_zFill{"WorldZFill", {0x89}, {0xC3}, 1};                // mov [rsp+0x10],edx -> ret
CodePatch g_dispBatch{"DispBatch", {0xFF, 0x50, 0x60}, {0x0F, 0x1F, 0x00}, 3}; // call [rax+0x60] -> nop
CodePatch *const g_codePatches[] = {&g_drawChains, &g_zFill, &g_dispBatch};

void arm(CodePatch &patch, uint8_t *at) {
  DWORD old;
  if (!g_image.code(at, patch.size) || std::memcmp(at, patch.original, patch.size) != 0) {
    patch.state = "bytes differ";
  } else if (!VirtualProtect(at, patch.size, PAGE_EXECUTE_READWRITE, &old)) {
    patch.state = "cannot make writable";
  } else {
    patch.at = at;
    patch.usable = true;
    patch.state = "ok";
  }
}

void flip(CodePatch &patch, bool on) {
  if (!patch.usable || patch.applied == on)
    return;
  // Whatever else rewrote these bytes owns them now.
  if (std::memcmp(patch.at, on ? patch.original : patch.patched, patch.size) != 0) {
    patch.usable = patch.applied = false;
    patch.state = "bytes changed at runtime";
    return;
  }
  std::memcpy(patch.at, on ? patch.patched : patch.original, patch.size);
  FlushInstructionCache(GetCurrentProcess(), patch.at, patch.size);
  patch.applied = on;
}

struct VtableHook {
  const char *name;
  void **slot = nullptr;
  std::atomic<const char *> state{"not located"};
};
VtableHook g_propsHook{"DrawStaticProps"}, g_overlaysHook{"RenderOverlays"};

// One pointer-sized write: callers on other threads see either function.
bool swapSlot(void **slot, void *hook) {
  DWORD old;
  if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old))
    return false;
  InterlockedExchangePointer(slot, hook);
  VirtualProtect(slot, sizeof(void *), old, &old);
  return true;
}

std::atomic<bool> g_ready{false};
std::atomic<const char *> g_status{"not installed"};

struct Statistics {
  std::atomic<uint64_t> views{0}, skippedViews{0}, propCalls{0}, propsFiltered{0}, propsPassed{0},
      propsUnknown{0}, overlaysSkipped{0}, overlaysPassed{0};
} g_stats;

// Whether the engine's static content is redundant right now. With a 3D
// skybox the sky camera rasterizes (OPENREMIX_PROXY_SKY_TRACE=0) the engine's
// sky view still has to draw its world and props, and that view cannot be
// told apart here: nothing is skipped on such a map. The BSP state is written
// by the render thread; these are plain reads of scalars in static storage,
// at worst a frame stale across a map change.
bool redundant() {
  if (!g_ready.load(std::memory_order_acquire) || !bsp::active())
    return false;
  float origin[3], scale;
  return bsp::skyTracedReach() > 0 || !bsp::skyCamera(origin, scale);
}

// CStaticProp's IClientRenderable: GetRenderOrigin (slot 1) and
// RenderableToWorldTransform (slot 34) return members, `lea rax,[rcx+d8]; ret`.
// Read from the code, so a changed layout is noticed instead of misread.
// Only a vtable that passed is remembered (props may be drawn from more than
// one thread: the offsets are published before it).
std::atomic<void **> g_propVtable{nullptr};
std::atomic<int8_t> g_propOrigin{0}, g_propRows{0};
std::atomic<bool> g_propFilterBroken{false};

bool memberGetter(void *function, int8_t &offset) {
  if (!codeMatches(function, "48 8D 41 ? C3"))
    return false;
  offset = int8_t(static_cast<uint8_t *>(function)[3]);
  return offset > 0;
}

// The prop's model-to-world rows, or null when it is not a CStaticProp.
const float *propTransform(void *prop) {
  void **vtable = *static_cast<void ***>(prop);
  if (vtable != g_propVtable.load(std::memory_order_acquire)) {
    int8_t originOffset, rowsOffset;
    if (!g_image.constant(vtable, 35 * sizeof(void *)) || !memberGetter(vtable[1], originOffset) ||
        !memberGetter(vtable[34], rowsOffset))
      return nullptr;
    g_propOrigin = originOffset;
    g_propRows = rowsOffset;
    g_propVtable.store(vtable, std::memory_order_release);
  }
  const float *origin = reinterpret_cast<const float *>(static_cast<uint8_t *>(prop) + g_propOrigin.load());
  const float *rows = reinterpret_cast<const float *>(static_cast<uint8_t *>(prop) + g_propRows.load());
  // The rows' translation column is the render origin, or this is not the
  // matrix it is taken for.
  for (int k = 0; k < 3; ++k)
    if (!(std::fabs(rows[k * 4 + 3] - origin[k]) < .01f)) {
      g_propFilterBroken = true;
      return nullptr;
    }
  return rows;
}

// The bools stay as the caller left them in their registers and stack slot.
using DrawStaticPropsFn = void (*)(void *self, void **props, int count, uintptr_t shadowDepth, uintptr_t wireframe);
DrawStaticPropsFn g_drawStaticProps = nullptr;

void drawStaticProps(void *self, void **props, int count, uintptr_t shadowDepth, uintptr_t wireframe) {
  if ((shadowDepth & 0xff) || count <= 0 || !props || g_propFilterBroken.load(std::memory_order_relaxed) ||
      !redundant()) {
    g_drawStaticProps(self, props, count, shadowDepth, wireframe);
    return;
  }
  void *local[1024];
  std::vector<void *> large;
  void **kept = local;
  if (size_t(count) > sizeof(local) / sizeof(local[0])) {
    large.resize(size_t(count));
    kept = large.data();
  }
  int keptCount = 0, unknown = 0;
  for (int i = 0; i < count; ++i) {
    const float *rows = props[i] ? propTransform(props[i]) : nullptr;
    if (!rows)
      ++unknown;
    if (!rows || !bsp::providesProp(rows))
      kept[keptCount++] = props[i];
  }
  if (g_propFilterBroken.load(std::memory_order_relaxed)) {
    g_propsHook.state = "filter off: transform is not at the expected member";
    g_drawStaticProps(self, props, count, shadowDepth, wireframe);
    return;
  }
  g_stats.propCalls.fetch_add(1, std::memory_order_relaxed);
  g_stats.propsFiltered.fetch_add(uint64_t(count - keptCount), std::memory_order_relaxed);
  g_stats.propsPassed.fetch_add(uint64_t(keptCount), std::memory_order_relaxed);
  g_stats.propsUnknown.fetch_add(uint64_t(unknown), std::memory_order_relaxed);
  if (keptCount)
    g_drawStaticProps(self, kept, keptCount, shadowDepth, wireframe);
}

bool envDisabled(const char *name) {
  char v[8] = {};
  return GetEnvironmentVariableA(name, v, sizeof(v)) && v[0] == '0';
}

void locateCodePatches(uint8_t *worldEnd) {
  uint8_t *found = nullptr, *called = nullptr;
  const size_t chains = scanCode(kDrawChainsEntry, found);
  if (chains != 1)
    g_drawChains.state = chains ? "signature not unique" : "signature not found";
  else if (worldEnd && (calledFromWorldEnd(worldEnd, kDrawChainsEntry, called) != 1 || called != found))
    g_drawChains.state = "not the function Shader_WorldEnd calls";
  else
    arm(g_drawChains, found);

  if (!worldEnd)
    g_zFill.state = "call chain not decoded";
  else if (calledFromWorldEnd(worldEnd, kZFillEntry, called) != 1)
    g_zFill.state = "not called from Shader_WorldEnd";
  else
    arm(g_zFill, called);

  const size_t sites = scanCode(kDispBatchSite, found);
  if (sites != 1)
    g_dispBatch.state = sites ? "signature not unique" : "signature not found";
  else
    arm(g_dispBatch, found + kDispBatchCall);
}

void hookStaticProps() {
  uint8_t *function = nullptr;
  const size_t count = scanCode(kDrawStaticPropsEntry, function);
  if (count != 1) {
    g_propsHook.state = count ? "signature not unique" : "signature not found";
    return;
  }
  using CreateInterfaceFn = void *(*)(const char *name, int *returnCode);
  auto factory = reinterpret_cast<CreateInterfaceFn>(reinterpret_cast<void *>(
      GetProcAddress(reinterpret_cast<HMODULE>(g_image.base), "CreateInterface")));
  void *manager = factory ? factory("StaticPropMgrClient004", nullptr) : nullptr;
  if (!manager || !g_image.inside(manager, sizeof(void *))) {
    g_propsHook.state = "interface not found";
    return;
  }
  void **vtable = *static_cast<void ***>(manager);
  if (!g_image.constant(vtable, (kDrawStaticPropsSlot + 1) * sizeof(void *)) ||
      vtable[kDrawStaticPropsSlot] != function) {
    g_propsHook.state = "vtable slot is not DrawStaticProps";
    return;
  }
  g_drawStaticProps = reinterpret_cast<DrawStaticPropsFn>(function);
  g_propsHook.slot = &vtable[kDrawStaticPropsSlot];
  g_propsHook.state = swapSlot(g_propsHook.slot, reinterpret_cast<void *>(&drawStaticProps)) ? "ok"
                                                                                               : "cannot write vtable";
}

} // namespace
} // namespace proxy::engine_patches

// COverlayMgr::RenderOverlays: returns at once while the BSP world draws the
// map's overlays (the callers clear the render lists themselves), otherwise
// continues in the engine's function with every argument untouched.
extern "C" {
void *openremix_renderOverlaysOriginal = nullptr;
int openremix_skipOverlays() {
  using namespace proxy::engine_patches;
  const bool skip = redundant() && proxy::bsp::overlaysActive();
  ++(skip ? g_stats.overlaysSkipped : g_stats.overlaysPassed);
  return skip;
}
void openremix_renderOverlays();
}
asm(R"(
  .text
  .globl openremix_renderOverlays
openremix_renderOverlays:
  .intel_syntax noprefix
  push rbp
  mov rbp, rsp
  sub rsp, 0x20
  mov [rbp+0x10], rcx
  mov [rbp+0x18], rdx
  mov [rbp+0x20], r8
  mov [rbp+0x28], r9
  call openremix_skipOverlays
  mov rcx, [rbp+0x10]
  mov rdx, [rbp+0x18]
  mov r8, [rbp+0x20]
  mov r9, [rbp+0x28]
  test eax, eax
  leave
  jnz 1f
  jmp [rip+openremix_renderOverlaysOriginal]
1:
  ret
  .att_syntax prefix
)");

namespace proxy::engine_patches {
namespace {

void hookOverlays(uint8_t *worldEnd) {
  uint8_t *function = nullptr;
  const size_t count = scanCode(kRenderOverlaysEntry, function);
  if (count != 1) {
    g_overlaysHook.state = count ? "signature not unique" : "signature not found";
    return;
  }
  // The class has one instance, so its vtable is patched in place.
  void **slot = nullptr;
  if (scanPointers(function, slot) != 1) {
    g_overlaysHook.state = "no single vtable slot";
    return;
  }
  if (!worldEnd) {
    g_overlaysHook.state = "call chain not decoded";
    return;
  }
  // Shader_WorldEnd calls OverlayMgr() (`lea rax,[mgr]; ret`): its object
  // must use this vtable, with the function in the RenderOverlays slot.
  bool confirmed = false;
  for (size_t i = 0; i + 5 <= kWorldEndSpan && !confirmed; ++i) {
    if (worldEnd[i] != 0xE8)
      continue;
    uint8_t *accessor = relative(worldEnd + i + 5);
    if (!codeMatches(accessor, "48 8D 05 ? ? ? ? C3"))
      continue;
    uint8_t *manager = relative(accessor + 7);
    confirmed = g_image.inside(manager, sizeof(void *)) &&
                *reinterpret_cast<void ***>(manager) == slot - kRenderOverlaysSlot;
  }
  if (!confirmed) {
    g_overlaysHook.state = "not OverlayMgr()'s vtable";
    return;
  }
  openremix_renderOverlaysOriginal = function;
  g_overlaysHook.slot = slot;
  g_overlaysHook.state = swapSlot(slot, reinterpret_cast<void *>(&openremix_renderOverlays)) ? "ok"
                                                                                               : "cannot write vtable";
}

} // namespace

bool install() {
  static bool attempted = false;
  if (attempted)
    return g_ready;
  attempted = true;
  // OPENREMIX_PROXY_ENGINE_SKIP=0 leaves the engine drawing everything (the
  // proxy then drops the redundant draws itself).
  if (envDisabled("OPENREMIX_PROXY_ENGINE_SKIP") || envDisabled("OPENREMIX_PROXY_ENGINE_HOOKS")) {
    g_status = "disabled by environment";
    return false;
  }
  if (!openImage()) {
    g_status = "engine.dll not readable";
    return false;
  }
  // Nothing is rewritten here except the two vtable slots (one pointer
  // each): the main thread may be anywhere in the engine meanwhile.
  uint8_t *worldEnd = findWorldEnd();
  locateCodePatches(worldEnd);
  hookStaticProps();
  hookOverlays(worldEnd);
  g_status = worldEnd ? "installed" : "installed, call chain not decoded";
  g_ready.store(true, std::memory_order_release);
  return true;
}

// Stateless per call (no depth counter): the engine does not nest
// DrawWorldLists, and should it ever leave one without returning (Host_Error
// unwinds with longjmp), the next view's begin or end puts the code right.
void beginWorldLists() {
  if (!g_ready.load(std::memory_order_acquire))
    return;
  const bool skip = redundant();
  g_stats.views.fetch_add(1, std::memory_order_relaxed);
  if (skip)
    g_stats.skippedViews.fetch_add(1, std::memory_order_relaxed);
  for (CodePatch *patch : g_codePatches)
    flip(*patch, skip);
}

void endWorldLists() {
  if (!g_ready.load(std::memory_order_acquire))
    return;
  for (CodePatch *patch : g_codePatches)
    flip(*patch, false);
}

void dumpStatistics(FILE *out) {
  fprintf(out, "== Engine patches: %s", g_status.load());
  if (!g_ready.load(std::memory_order_acquire)) {
    fprintf(out, "\n");
    return;
  }
  fprintf(out, " | skipping now: %d\n", int(redundant()));
  for (const CodePatch *patch : g_codePatches)
    fprintf(out, "   %s: %s (engine+0x%llX)\n", patch->name, patch->state.load(),
            (unsigned long long)(patch->at ? patch->at - g_image.base : 0));
  for (const VtableHook *hook : {&g_propsHook, &g_overlaysHook})
    fprintf(out, "   %s: %s (slot at engine+0x%llX)\n", hook->name, hook->state.load(),
            (unsigned long long)(hook->slot ? reinterpret_cast<uint8_t *>(hook->slot) - g_image.base : 0));
  static ULONGLONG last = GetTickCount64();
  const ULONGLONG now = GetTickCount64();
  const double seconds = now > last ? double(now - last) / 1000 : 1;
  last = now;
  const uint64_t views = g_stats.views.exchange(0), skipped = g_stats.skippedViews.exchange(0);
  const uint64_t calls = g_stats.propCalls.exchange(0);
  fprintf(out,
          "   per second: world-list views %.1f, with the engine's static world skipped %.1f | static prop calls "
          "%.1f: props filtered %.1f, passed %.1f (not static props %.1f) | overlay renders skipped %.1f, passed "
          "%.1f\n",
          double(views) / seconds, double(skipped) / seconds, double(calls) / seconds,
          double(g_stats.propsFiltered.exchange(0)) / seconds, double(g_stats.propsPassed.exchange(0)) / seconds,
          double(g_stats.propsUnknown.exchange(0)) / seconds, double(g_stats.overlaysSkipped.exchange(0)) / seconds,
          double(g_stats.overlaysPassed.exchange(0)) / seconds);
}

} // namespace proxy::engine_patches
