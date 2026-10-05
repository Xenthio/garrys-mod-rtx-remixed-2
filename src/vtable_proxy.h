#pragma once
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Forwarding wrappers for interfaces we do not have headers for (x64).
//
// A wrapper is { vtable, real object }. Every vtable slot starts as a generated
// thunk that swaps RCX (`this`) for the real object and jumps to the real
// vtable's slot, so any method is forwarded without knowing its signature.
// Individual slots can then be replaced with typed hooks (see setHook).
namespace proxy {

struct Wrapper {
  const void *const *vtbl;
  void *real;
  struct Kind *kind;
};

struct Kind {
  std::string name;
  uint32_t slotCount = 0;
  const void *const *realVtbl = nullptr; // vtable of the first object wrapped with this kind
  void **vtbl = nullptr;                 // wrapper vtable (thunks, or hooks)
  uint32_t *calls = nullptr;             // per-slot call counters
  std::vector<std::string> slotNames;    // optional, for statistics
};

// Creates (once per real vtable) a kind whose slots forward to that vtable.
// `created` reports whether this call made the kind (so hooks are set once).
Kind *kindFor(const char *name, const void *const *realVtbl, uint32_t slotCount, bool *created = nullptr);

// Returns the wrapper for `real`, creating it on first use. Wrappers live for
// the process lifetime (interfaces are singletons).
void *wrap(Kind *kind, void *real);
void *unwrapIf(void *maybeWrapper);

// Replaces a slot with a typed hook: a plain function taking the wrapper as
// its first parameter followed by the method's exact parameters (x64 member
// functions use the ordinary calling convention with `this` in RCX).
void setHook(Kind *kind, uint32_t slot, void *hook);

// Original method for a hook's `this`.
template <class Fn> Fn original(const Wrapper *self, uint32_t slot) {
  return reinterpret_cast<Fn>(const_cast<void *>(self->kind->realVtbl[slot]));
}

std::vector<std::pair<void *, const char *>> realObjects();
void dumpStatistics(FILE *out);

} // namespace proxy
