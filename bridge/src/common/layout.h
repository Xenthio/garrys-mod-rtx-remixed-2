#pragma once
#include <cstdint>

// Struct layout tables shared by the x86 client (encoder) and the x64 host
// (pointer/handle fix-up). Generated tables live in layouts.gen.h.
namespace bridge::layout {

enum FieldKind : uint8_t {
  F_POD,             // plain bytes, same size on both ABIs
  F_NESTED,          // embedded struct whose layout differs; aux = StructId
  F_PNEXT,           // sType-tagged extension chain
  F_WSTR,            // remixapi_Path (NUL-terminated wchar_t, 2 bytes on Windows)
  F_CSTR,            // NUL-terminated char string
  F_HANDLE_MATERIAL, // client-side handle ids (pointer-sized), translated by the host
  F_HANDLE_MESH,
  F_HANDLE_HOST_SHADER, // uint64_t client-side id, translated by the host
  F_HWND,            // window handles are 32-bit values valid in every process
  F_ARRAY,           // POD elements; aux = element size, link = count field
  F_ARRAY_STRUCT,    // struct elements; aux = StructId, link = count field
  F_BLOB,            // raw bytes; link = byte size field
  F_OPTIONAL,        // pointer to one POD value; aux = value size
  F_NULLPTR,         // not transportable (function pointers, D3D objects)
};

struct FieldDesc {
  uint16_t off32;
  uint16_t off64;
  uint16_t size64;
  FieldKind kind;
  uint16_t aux;
  uint16_t link;
};

struct StructDesc {
  const char *name;
  uint32_t sType; // 0 when the struct is not an sType-tagged chain member
  uint16_t size32;
  uint16_t size64;
  uint16_t align32;
  uint16_t align64;
  uint16_t fieldCount;
  const FieldDesc *fields;
};

} // namespace bridge::layout
