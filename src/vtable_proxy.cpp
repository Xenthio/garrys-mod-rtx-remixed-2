#include "vtable_proxy.h"

#include <cstring>
#include <map>
#include <mutex>

namespace proxy {
namespace {

std::mutex g_lock;
std::map<const void *, Kind *> g_kinds; // real vtable -> kind
std::map<void *, Wrapper *> g_wrappers; // real object -> wrapper

// Per slot (32 bytes, x64):
//   48 B8 <counter64>   mov  rax, counter
//   FF 00               inc  dword [rax]         ; statistics, not atomic
//   48 8B 49 08         mov  rcx, [rcx+8]        ; Wrapper::real
//   48 8B 01            mov  rax, [rcx]          ; real vtable
//   FF A0 <disp32>      jmp  [rax + slot*8]
// RCX is `this`; the other argument registers, the stack and the return
// address are untouched, so any signature forwards.
constexpr size_t kThunkSize = 32;

void *makeThunks(uint32_t count, uint32_t *counters) {
  auto *code = static_cast<uint8_t *>(
      VirtualAlloc(nullptr, count * kThunkSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  for (uint32_t i = 0; i < count; ++i) {
    uint8_t *t = code + i * kThunkSize;
    const uint64_t counter = reinterpret_cast<uint64_t>(&counters[i]);
    const uint32_t disp = i * 8;
    size_t n = 0;
    t[n++] = 0x48;
    t[n++] = 0xB8;
    std::memcpy(t + n, &counter, 8);
    n += 8;
    t[n++] = 0xFF;
    t[n++] = 0x00;
    t[n++] = 0x48;
    t[n++] = 0x8B;
    t[n++] = 0x49;
    t[n++] = 0x08;
    t[n++] = 0x48;
    t[n++] = 0x8B;
    t[n++] = 0x01;
    t[n++] = 0xFF;
    t[n++] = 0xA0;
    std::memcpy(t + n, &disp, 4);
    n += 4;
    while (n < kThunkSize)
      t[n++] = 0xCC;
  }
  DWORD old;
  VirtualProtect(code, count * kThunkSize, PAGE_EXECUTE_READ, &old);
  FlushInstructionCache(GetCurrentProcess(), code, count * kThunkSize);
  return code;
}

} // namespace

Kind *kindFor(const char *name, const void *const *realVtbl, uint32_t slotCount, bool *created) {
  std::lock_guard<std::mutex> lock(g_lock);
  auto it = g_kinds.find(realVtbl);
  if (created)
    *created = it == g_kinds.end();
  if (it != g_kinds.end())
    return it->second;
  auto *kind = new Kind();
  kind->name = name;
  kind->slotCount = slotCount;
  kind->realVtbl = realVtbl;
  kind->calls = new uint32_t[slotCount]();
  kind->vtbl = new void *[slotCount];
  auto *thunks = static_cast<uint8_t *>(makeThunks(slotCount, kind->calls));
  for (uint32_t i = 0; i < slotCount; ++i)
    kind->vtbl[i] = thunks + i * kThunkSize;
  g_kinds[realVtbl] = kind;
  return kind;
}

void *wrap(Kind *kind, void *real) {
  if (!real)
    return nullptr;
  std::lock_guard<std::mutex> lock(g_lock);
  auto it = g_wrappers.find(real);
  if (it != g_wrappers.end())
    return it->second;
  auto *w = new Wrapper{kind->vtbl, real, kind};
  g_wrappers[real] = w;
  return w;
}

void *unwrapIf(void *maybeWrapper) {
  std::lock_guard<std::mutex> lock(g_lock);
  for (auto &[real, w] : g_wrappers)
    if (w == maybeWrapper)
      return real;
  return maybeWrapper;
}

void setHook(Kind *kind, uint32_t slot, void *hook) {
  if (slot < kind->slotCount)
    kind->vtbl[slot] = hook;
}

std::vector<std::pair<void *, const char *>> realObjects() {
  std::lock_guard<std::mutex> lock(g_lock);
  std::vector<std::pair<void *, const char *>> out;
  for (auto &[real, w] : g_wrappers)
    out.emplace_back(real, w->kind->name.c_str());
  return out;
}

void dumpStatistics(FILE *out) {
  std::lock_guard<std::mutex> lock(g_lock);
  for (auto &[vt, kind] : g_kinds) {
    fprintf(out, "== %s (%u slots)\n", kind->name.c_str(), kind->slotCount);
    for (uint32_t i = 0; i < kind->slotCount; ++i) {
      if (!kind->calls[i])
        continue;
      const char *name = i < kind->slotNames.size() ? kind->slotNames[i].c_str() : "";
      fprintf(out, "  [%3u] %10u  %s\n", i, kind->calls[i], name);
    }
  }
  fflush(out);
}

} // namespace proxy
