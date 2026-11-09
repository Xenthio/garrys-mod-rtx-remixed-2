#pragma once
#include "client/transport.h"
#include "common/layouts.gen.h"
#include "common/protocol.h"

#include <cstring>
#include <vector>

namespace bridge::client {

// Builds one record: Record header, fixed argument block, then x64 images of
// struct arguments and their inline payloads. Offsets are record-relative so
// the buffer can grow while encoding.
class Encoder {
public:
  explicit Encoder(Transport &transport, Log &log) : m_transport(transport), m_log(log) {}

  void reset(proto::Op op, uint32_t argsSize);
  template <class A> void setArgs(const A &args) {
    std::memcpy(m_buf.data() + sizeof(proto::Record), &args, sizeof(A));
  }

  uint64_t structure(layout::StructId id, const void *src);
  // `count` consecutive structs (an array argument).
  uint64_t structures(layout::StructId id, const void *src, uint32_t count);
  uint64_t chain(const void *pNext);
  // rowCount rows of rowBytes, `pitch` apart in the source, sent tightly packed.
  uint64_t rows(const void *data, size_t rowBytes, uint32_t rowCount, size_t pitch);
  uint64_t bytes(const void *data, size_t size, size_t align = 16);
  uint64_t cstr(const char *s) { return s ? bytes(s, std::strlen(s) + 1, 1) : 0; }
  uint64_t wstr(const wchar_t *s) { return s ? bytes(s, (std::wcslen(s) + 1) * sizeof(wchar_t), 2) : 0; }

  std::vector<uint8_t> &buffer() { return m_buf; }

private:
  size_t alloc(size_t size, size_t align);
  void encodeInto(layout::StructId id, const uint8_t *src, size_t dst);
  void put64(size_t at, uint64_t v) { std::memcpy(m_buf.data() + at, &v, 8); }

  Transport &m_transport;
  Log &m_log;
  std::vector<uint8_t> m_buf;
  std::vector<uint8_t> m_scratch; // rows(): repacked payloads sent outside the record
  uint32_t m_warnedSTypes[16] = {};
};

// Lookup from sType to struct id for pNext chains.
bool structForSType(uint32_t sType, layout::StructId &out);

} // namespace bridge::client
