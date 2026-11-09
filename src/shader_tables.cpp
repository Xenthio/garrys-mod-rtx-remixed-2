#include "shader_tables.h"

#include <cstring>

namespace proxy::shader_tables {

bool parse(const std::vector<uint32_t> &code, ConstantTable &out) {
  constexpr uint32_t kCtab = 0x42415443; // "CTAB"
  for (size_t i = 1; i < code.size();) {
    const uint32_t token = code[i];
    if (token == 0x0000ffff)
      break; // end
    if ((token & 0xffff) == 0xfffe) { // comment
      const size_t words = (token >> 16) & 0x7fff;
      if (words >= 8 && i + 1 + words <= code.size() && code[i + 1] == kCtab) {
        const auto *table = reinterpret_cast<const uint8_t *>(&code[i + 2]);
        const size_t bytes = (words - 1) * 4;
        uint32_t header[7];
        if (bytes < sizeof(header))
          return false;
        std::memcpy(header, table, sizeof(header));
        const uint32_t constants = header[3], info = header[4];
        if (info > bytes || uint64_t(constants) * 20 > bytes - info)
          return false;
        for (uint32_t c = 0; c < constants; ++c) {
          const uint8_t *e = table + info + size_t(c) * 20;
          uint32_t nameAt;
          uint16_t set, index, count;
          std::memcpy(&nameAt, e, 4);
          std::memcpy(&set, e + 4, 2);
          std::memcpy(&index, e + 6, 2);
          std::memcpy(&count, e + 8, 2);
          if (nameAt >= bytes)
            continue;
          const char *name = reinterpret_cast<const char *>(table + nameAt);
          const size_t length = strnlen(name, bytes - nameAt);
          out.entries.push_back({std::string(name, length), set, index, count});
        }
        return true;
      }
      i += 1 + words;
      continue;
    }
    i += 1 + ((token >> 24) & 0xf); // SM2+: instruction length in the opcode token
  }
  return false;
}

} // namespace proxy::shader_tables
