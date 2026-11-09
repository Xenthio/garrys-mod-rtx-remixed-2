#include "client/encoder.h"

static_assert(sizeof(void *) == 4, "the bridge client encodes 32-bit layouts");

namespace bridge::client {
using namespace bridge::layout;
using namespace bridge::proto;

namespace {
// Payloads above this size bypass the record (DATA record or blob).
constexpr size_t kInlineLimit = 64 * 1024;

uint32_t read32(const uint8_t *p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

uint64_t readCount(const uint8_t *src, const FieldDesc &f) {
  if (f.size64 == 8) {
    uint64_t v;
    std::memcpy(&v, src + f.off32, 8);
    return v;
  }
  return read32(src + f.off32);
}

const void *readPtr(const uint8_t *p) { return reinterpret_cast<const void *>(static_cast<uintptr_t>(read32(p))); }
} // namespace

bool structForSType(uint32_t sType, StructId &out) {
  if (sType == 0)
    return false;
  for (uint16_t i = 0; i < S_COUNT; ++i) {
    if (kStructs[i].sType == sType) {
      out = static_cast<StructId>(i);
      return true;
    }
  }
  return false;
}

void Encoder::reset(Op op, uint32_t argsSize) {
  m_buf.assign(sizeof(Record) + argsSize, 0);
  auto *rec = reinterpret_cast<Record *>(m_buf.data());
  rec->op = op;
}

size_t Encoder::alloc(size_t size, size_t align) {
  size_t at = (m_buf.size() + align - 1) & ~(align - 1);
  m_buf.resize(at + size, 0);
  return at;
}

uint64_t Encoder::bytes(const void *data, size_t size, size_t align) {
  if (!data)
    return 0;
  if (size > kInlineLimit)
    return m_transport.sendLarge(data, size);
  size_t at = alloc(size, align < 16 ? align : 16);
  std::memcpy(m_buf.data() + at, data, size);
  return kPtrRecord | at;
}

uint64_t Encoder::structure(StructId id, const void *src) {
  if (!src)
    return 0;
  const StructDesc &d = kStructs[id];
  size_t at = alloc(d.size64, d.align64);
  encodeInto(id, static_cast<const uint8_t *>(src), at);
  return kPtrRecord | at;
}

uint64_t Encoder::structures(StructId id, const void *src, uint32_t count) {
  if (!src || !count)
    return 0;
  const StructDesc &d = kStructs[id];
  const size_t at = alloc(size_t(count) * d.size64, d.align64);
  for (uint32_t k = 0; k < count; ++k)
    encodeInto(id, static_cast<const uint8_t *>(src) + size_t(k) * d.size32, at + size_t(k) * d.size64);
  return kPtrRecord | at;
}

uint64_t Encoder::rows(const void *data, size_t rowBytes, uint32_t rowCount, size_t pitch) {
  if (!data || !rowBytes || !rowCount)
    return 0;
  const auto *src = static_cast<const uint8_t *>(data);
  if (pitch == rowBytes)
    return bytes(src, rowBytes * rowCount);
  const size_t total = rowBytes * rowCount;
  uint8_t *dst;
  size_t at = 0;
  if (total > kInlineLimit) {
    m_scratch.resize(total);
    dst = m_scratch.data();
  } else {
    at = alloc(total, 16);
    dst = m_buf.data() + at;
  }
  for (uint32_t y = 0; y < rowCount; ++y)
    std::memcpy(dst + y * rowBytes, src + y * pitch, rowBytes);
  return total > kInlineLimit ? m_transport.sendLarge(m_scratch.data(), total) : (kPtrRecord | at);
}

uint64_t Encoder::chain(const void *next) {
  while (next) {
    auto *p = static_cast<const uint8_t *>(next);
    const uint32_t sType = read32(p);
    StructId id;
    if (structForSType(sType, id))
      return structure(id, p);
    bool known = false;
    for (uint32_t &w : m_warnedSTypes) {
      if (w == sType) {
        known = true;
        break;
      }
      if (w == 0) {
        w = sType;
        break;
      }
    }
    if (!known)
      m_log.write("bridge: dropping unsupported pNext sType 0x%x", sType);
    next = readPtr(p + 4); // every chained struct starts with sType, pNext
  }
  return 0;
}

void Encoder::encodeInto(StructId id, const uint8_t *src, size_t dst) {
  const StructDesc &d = kStructs[id];
  for (uint16_t i = 0; i < d.fieldCount; ++i) {
    const FieldDesc &f = d.fields[i];
    const uint8_t *s = src + f.off32;
    const size_t o = dst + f.off64;
    switch (f.kind) {
    case F_POD:
      std::memcpy(m_buf.data() + o, s, f.size64);
      break;
    case F_NESTED:
      encodeInto(static_cast<StructId>(f.aux), s, o);
      break;
    case F_PNEXT:
      put64(o, chain(readPtr(s)));
      break;
    case F_WSTR:
      put64(o, wstr(static_cast<const wchar_t *>(readPtr(s))));
      break;
    case F_CSTR:
      put64(o, cstr(static_cast<const char *>(readPtr(s))));
      break;
    case F_HANDLE_MATERIAL:
    case F_HANDLE_MESH:
      put64(o, read32(s)); // client-side id
      break;
    case F_HANDLE_HOST_SHADER:
      std::memcpy(m_buf.data() + o, s, 8); // client-side id
      break;
    case F_HWND:
      put64(o, static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(read32(s)))));
      break;
    case F_ARRAY:
      put64(o, bytes(readPtr(s), readCount(src, d.fields[f.link]) * f.aux, 16));
      break;
    case F_ARRAY_STRUCT: {
      const void *items = readPtr(s);
      const uint64_t count = readCount(src, d.fields[f.link]);
      if (!items || !count) {
        put64(o, 0);
        break;
      }
      const StructDesc &e = kStructs[f.aux];
      const size_t at = alloc(size_t(count) * e.size64, e.align64);
      for (uint64_t k = 0; k < count; ++k)
        encodeInto(static_cast<StructId>(f.aux), static_cast<const uint8_t *>(items) + k * e.size32,
                   at + k * e.size64);
      put64(o, kPtrRecord | at);
      break;
    }
    case F_BLOB:
      put64(o, bytes(readPtr(s), readCount(src, d.fields[f.link]), 16));
      break;
    case F_OPTIONAL:
      put64(o, bytes(readPtr(s), f.aux, 8));
      break;
    case F_NULLPTR:
      put64(o, 0);
      break;
    }
  }
}

} // namespace bridge::client
