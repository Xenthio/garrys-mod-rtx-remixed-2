#pragma once

#include <windows.h>

#include <cstdint>

// The engine modules of the games whose builds are found instead of assumed
// (L4D2 and Half-Life 2 RTX, PE32; TF2, PE32+): what the proxy reads of them.
// Code by a byte pattern or checked against the bytes it expects, classes by
// their MSVC RTTI. Was x86_module.h, before that l4d2.h's (L4D2 names them
// through using-declarations there).
namespace proxy::pe {

// A module loaded in this process (PE32 in the 32-bit proxy, PE32+ in the
// 64-bit one).
struct Module {
  const char *name = "";
  uintptr_t base = 0, size = 0;
  uintptr_t codeBegin = 0, codeEnd = 0; // RVAs of the first executable section
  bool open(const char *module);
  bool has(uintptr_t rva, size_t n) const { return base && rva <= size && n <= size - rva; }
  template <typename T> T *at(uintptr_t rva) const { return reinterpret_cast<T *>(base + rva); }
  bool holds(const void *p) const {
    const auto a = reinterpret_cast<uintptr_t>(p);
    return base && a >= base && a - base < size;
  }
  bool inCode(const void *p) const {
    const auto a = reinterpret_cast<uintptr_t>(p);
    return base && a >= base + codeBegin && a < base + codeEnd;
  }
};

// A byte pattern ("8B 50 ? 8B CB", ? any byte) found in a module's code.
struct Pattern {
  const char *name;
  const char *bytes;
  uintptr_t staticRva; // where it is in the build this was written for
  uintptr_t rva = 0;   // first match (0: none)
  unsigned count = 0;  // matches in the code section
  // Scans the module's code; true when found.
  bool find(const Module &module);
};

// Whether `bytes` (as Pattern::bytes) are at module+rva.
bool bytesAt(const Module &module, uintptr_t rva, const char *bytes);
// The vtable of the class with the MSVC RTTI type name `typeName`
// (".?AVCOverlayMgr@@"), complete object at offset 0; null if none.
void **rttiVtable(const Module &module, const char *typeName);
void *createInterface(const char *module, const char *name);
// One pointer-sized vtable write; returns the previous entry (null: failed).
void *swapSlot(void **slot, void *hook);
// Rewrites code (made writable and flushed).
bool writeCode(void *at, const void *bytes, size_t n);
// A relative jump (E9) at `at` to `target`, `n` >= 5 bytes (the rest nops).
bool writeJump(void *at, const void *target, size_t n);

// A networked variable's offset in its class (client.dll's receive tables,
// the same layout in L4D2 and Portal 2): RecvTable {props, count, decoder,
// name}; RecvProp (0x3C bytes) {name, .., data table +0x28, offset +0x2C,
// ..}. Depth first, base classes (and other nested tables) included; -1 when
// not found.
int findNetProp(const void *recvTable, const char *name);

// Environment: OPENREMIX_PROXY_<name>; `fallback` when unset.
bool envFlag(const char *name, bool fallback);
float envFloat(const char *name, float fallback);

} // namespace proxy::pe
