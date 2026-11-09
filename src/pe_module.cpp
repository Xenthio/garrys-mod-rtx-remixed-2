// The module helpers (pe_module.h).
#include "pe_module.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace proxy::pe {

bool Module::open(const char *module) {
  const auto m = reinterpret_cast<uintptr_t>(GetModuleHandleA(module));
  if (!m)
    return false;
  const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(m);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    return false;
  // The proxy's own format: PE32 (x86) or PE32+ (x64).
  const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(m + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC)
    return false;
  const auto *section = IMAGE_FIRST_SECTION(nt);
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
    if (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) {
      codeBegin = section->VirtualAddress;
      codeEnd = section->VirtualAddress + section->Misc.VirtualSize;
      break;
    }
  if (codeEnd <= codeBegin)
    return false;
  name = module;
  base = m;
  size = nt->OptionalHeader.SizeOfImage;
  return true;
}

namespace {
// "8B 50 ? 8B CB" -> bytes and a mask (false: any byte).
size_t parse(const char *text, uint8_t *bytes, bool *mask, size_t capacity) {
  size_t n = 0;
  for (const char *p = text; *p && n < capacity;) {
    while (*p == ' ')
      ++p;
    if (!*p)
      break;
    if (*p == '?') {
      while (*p == '?')
        ++p;
      bytes[n] = 0;
      mask[n++] = false;
      continue;
    }
    char *end;
    const unsigned long value = std::strtoul(p, &end, 16);
    if (end == p)
      return 0;
    bytes[n] = uint8_t(value);
    mask[n++] = true;
    p = end;
  }
  return n;
}
} // namespace

bool Pattern::find(const Module &module) {
  uint8_t bytes[128];
  bool mask[128];
  const size_t n = parse(this->bytes, bytes, mask, sizeof(bytes));
  rva = 0;
  count = 0;
  if (!n || !module.base || module.codeEnd - module.codeBegin < n)
    return false;
  const uint8_t *begin = module.at<uint8_t>(module.codeBegin);
  const uint8_t *last = module.at<uint8_t>(module.codeEnd - n);
  // Anchored on the first exact byte.
  size_t anchor = 0;
  while (anchor < n && !mask[anchor])
    ++anchor;
  for (const uint8_t *p = begin; p <= last;) {
    if (anchor < n) {
      const void *hit = std::memchr(p + anchor, bytes[anchor], size_t(last - p) + 1);
      if (!hit)
        break;
      p = static_cast<const uint8_t *>(hit) - anchor;
    }
    size_t i = 0;
    while (i < n && (!mask[i] || p[i] == bytes[i]))
      ++i;
    if (i == n) {
      if (!count++)
        rva = uintptr_t(p) - module.base;
    }
    ++p;
  }
  return count != 0;
}

bool bytesAt(const Module &module, uintptr_t rva, const char *text) {
  uint8_t bytes[128];
  bool mask[128];
  const size_t n = parse(text, bytes, mask, sizeof(bytes));
  if (!n || !module.has(rva, n))
    return false;
  const uint8_t *p = module.at<uint8_t>(rva);
  for (size_t i = 0; i < n; ++i)
    if (mask[i] && p[i] != bytes[i])
      return false;
  return true;
}

void **rttiVtable(const Module &module, const char *typeName) {
  // TypeDescriptor: {vtable, spare (two pointers), name[]};
  // CompleteObjectLocator: {signature, offset, cdOffset, TypeDescriptor,
  // ClassHierarchyDescriptor[, itself]}, 32-bit fields: in PE32 addresses
  // (signature 0), in PE32+ RVAs (signature 1, with the locator's own RVA).
  // The vtable's entry -1 points at the locator.
#ifdef _WIN64
  constexpr uint32_t kSignature = 1;
  constexpr size_t kLocatorBytes = 24;
#else
  constexpr uint32_t kSignature = 0;
  constexpr size_t kLocatorBytes = 20;
#endif
  const size_t length = std::strlen(typeName) + 1;
  const uint8_t *image = module.at<uint8_t>(0);
  for (uintptr_t at = module.codeEnd; at + length <= module.size; ++at) {
    if (image[at] != uint8_t(typeName[0]) || std::memcmp(image + at, typeName, length) != 0)
      continue;
    const uintptr_t descriptorRva = at - 2 * sizeof(void *);
#ifdef _WIN64
    const uint32_t descriptor = uint32_t(descriptorRva);
#else
    const uint32_t descriptor = uint32_t(module.base + descriptorRva);
#endif
    for (uintptr_t col = 0; col + kLocatorBytes <= module.size; col += 4) {
      const auto *locator = reinterpret_cast<const uint32_t *>(image + col);
      if (locator[3] != descriptor || locator[0] != kSignature || locator[1] != 0)
        continue;
#ifdef _WIN64
      if (locator[5] != col)
        continue;
#endif
      const uintptr_t colAddress = module.base + col;
      for (uintptr_t slot = 0; slot + 2 * sizeof(void *) <= module.size; slot += sizeof(void *)) {
        const auto *entry = reinterpret_cast<const uintptr_t *>(image + slot);
        if (entry[0] == colAddress && module.inCode(reinterpret_cast<void *>(entry[1])))
          return reinterpret_cast<void **>(module.base + slot + sizeof(void *));
      }
    }
    return nullptr;
  }
  return nullptr;
}

void *createInterface(const char *module, const char *name) {
  using CreateInterfaceFn = void *(*)(const char *, int *);
  HMODULE m = GetModuleHandleA(module);
  auto factory = m ? reinterpret_cast<CreateInterfaceFn>(reinterpret_cast<void *>(GetProcAddress(m, "CreateInterface"))) : nullptr;
  return factory ? factory(name, nullptr) : nullptr;
}

void *swapSlot(void **slot, void *hook) {
  DWORD old;
  if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old))
    return nullptr;
  void *previous = InterlockedExchangePointer(slot, hook);
  VirtualProtect(slot, sizeof(void *), old, &old);
  return previous;
}

bool writeCode(void *at, const void *bytes, size_t n) {
  DWORD old;
  if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old))
    return false;
  std::memcpy(at, bytes, n);
  VirtualProtect(at, n, old, &old);
  FlushInstructionCache(GetCurrentProcess(), at, n);
  return true;
}

bool writeJump(void *at, const void *target, size_t n) {
  uint8_t code[16];
  if (n < 5 || n > sizeof(code))
    return false;
  code[0] = 0xE9;
  const int32_t rel = int32_t(reinterpret_cast<uintptr_t>(target) - (reinterpret_cast<uintptr_t>(at) + 5));
  std::memcpy(code + 1, &rel, 4);
  std::memset(code + 5, 0x90, n - 5);
  return writeCode(at, code, n);
}

namespace {
template <typename T> T readAt(const void *p, int offset) {
  T value;
  std::memcpy(&value, static_cast<const uint8_t *>(p) + offset, sizeof(T));
  return value;
}
int findNetProp(const uint8_t *table, const char *name, int base, int depth) {
  if (!table || depth > 16)
    return -1;
  const uint8_t *props = readAt<const uint8_t *>(table, 0);
  const int count = readAt<int>(table, 4);
  for (int i = 0; i < count && i < 4096; ++i) {
    const uint8_t *prop = props + size_t(i) * 0x3C;
    const char *propName = readAt<const char *>(prop, 0);
    const int offset = readAt<int>(prop, 0x2C);
    if (propName && std::strcmp(propName, name) == 0)
      return base + offset;
    if (const uint8_t *child = readAt<const uint8_t *>(prop, 0x28))
      if (const int found = findNetProp(child, name, base + offset, depth + 1); found >= 0)
        return found;
  }
  return -1;
}
} // namespace

int findNetProp(const void *recvTable, const char *name) {
  return findNetProp(static_cast<const uint8_t *>(recvTable), name, 0, 0);
}

bool envFlag(const char *name, bool fallback) {
  char full[96], v[8] = {};
  std::snprintf(full, sizeof(full), "OPENREMIX_PROXY_%s", name);
  if (!GetEnvironmentVariableA(full, v, sizeof(v)))
    return fallback;
  return v[0] != '0';
}

float envFloat(const char *name, float fallback) {
  char full[96], v[32] = {};
  std::snprintf(full, sizeof(full), "OPENREMIX_PROXY_%s", name);
  return GetEnvironmentVariableA(full, v, sizeof(v)) ? float(std::atof(v)) : fallback;
}

} // namespace proxy::pe
