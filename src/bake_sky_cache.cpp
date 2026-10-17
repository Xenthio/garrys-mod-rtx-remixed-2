#include "bake_sky_cache.h"
#define XXH_INLINE_ALL
#include "xxhash.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace proxy::remix {
namespace {
constexpr char magic[8] = {'R', 'M', 'X', 'S', 'K', 'Y', '0', '1'};
constexpr uint64_t headerBytes = 36;
template <typename T> void read(std::ifstream &in, T &value) {
  in.read(reinterpret_cast<char *>(&value), sizeof(value));
}
template <typename T> void write(std::ofstream &out, const T &value) {
  out.write(reinterpret_cast<const char *>(&value), sizeof(value));
}
bool equivalent(const uint8_t *current, const std::vector<uint8_t> &previous) {
  uint64_t difference = 0;
  for (size_t i = 0; i < previous.size(); ++i) {
    const unsigned delta = unsigned(std::abs(int(current[i]) - int(previous[i])));
    if (delta > 1 || (i % 4 == 3 && delta))
      return false;
    difference += delta;
  }
  // At most 1/64 of an 8-bit RGB level on average. A uniform one-level
  // exposure/colour change fails, as does any larger local change.
  return difference <= (previous.size() / 4 * 3) / 64;
}
} // namespace

std::filesystem::path bakeSkyCachePath(uint64_t mapId) noexcept {
  try {
    const char *enabled = std::getenv("OPENREMIX_PROXY_BAKE_SKY_CACHE");
    if (!mapId || (enabled && enabled[0] == '0'))
      return {};
    const char *override = std::getenv("REMIX_BAKE_CACHE_DIR");
    const char *config = std::getenv("REMIX_CONFIG_DIR");
    const auto directory = override && *override ? std::filesystem::u8path(override)
        : (config && *config ? std::filesystem::u8path(config)
                            : std::filesystem::current_path() / "openremix") / "cache" / "forwardplus";
    std::ostringstream name;
    name << std::hex << mapId << ".sky";
    return directory / "sky" / name.str();
  } catch (const std::exception &) {
    return {};
  }
}

BakeSkyImage canonicalBakeSky(const remixapi_TextureInfo &source, float scale,
                              const std::filesystem::path &file) noexcept {
  BakeSkyImage result;
  if (file.empty() || !source.data || !source.width || !source.height || source.width > 8192 ||
      source.height > 8192 || source.depth != 1 || source.mipLevels != 1 ||
      source.format != REMIXAPI_FORMAT_R8G8B8A8_SRGB || !std::isfinite(scale) || scale < 0 ||
      source.dataSize != uint64_t(source.width) * source.height * 4)
    return result;
  const auto bytes = size_t(source.dataSize);
  const auto pixels = static_cast<const uint8_t *>(source.data);
  const uint32_t format = uint32_t(source.format), version = 1;
  const uint32_t dimensions[] = {source.width, source.height, format};
  const auto seed = XXH3_64bits(dimensions, sizeof(dimensions));
  result.hash = XXH3_64bits_withSeed(pixels, bytes, seed) | 1;
  try {
    if (std::filesystem::is_regular_file(file) && std::filesystem::file_size(file) == headerBytes + bytes) {
      std::ifstream in(file, std::ios::binary);
      in.exceptions(std::ios::failbit | std::ios::badbit);
      char signature[8];
      uint32_t storedVersion, width, height, storedFormat;
      float storedScale;
      uint64_t hash;
      in.read(signature, sizeof(signature));
      read(in, storedVersion);
      read(in, width);
      read(in, height);
      read(in, storedFormat);
      read(in, storedScale);
      read(in, hash);
      if (std::equal(std::begin(magic), std::end(magic), signature) && storedVersion == version &&
          width == source.width && height == source.height && storedFormat == format && storedScale == scale) {
        std::vector<uint8_t> previous(bytes);
        in.read(reinterpret_cast<char *>(previous.data()), bytes);
        if ((XXH3_64bits_withSeed(previous.data(), bytes, seed) | 1) == hash && equivalent(pixels, previous)) {
          result.pixels = std::move(previous);
          result.hash = hash;
          result.reused = true;
          return result;
        }
      }
    }
  } catch (const std::exception &) {
    // A corrupt/incomplete record is replaced from the current valid capture.
  }
  try {
    std::filesystem::create_directories(file.parent_path());
    auto temporary = file;
    temporary += "." + std::to_string(GetCurrentProcessId()) + ".tmp";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    out.exceptions(std::ios::failbit | std::ios::badbit);
    out.write(magic, sizeof(magic));
    write(out, version);
    write(out, source.width);
    write(out, source.height);
    write(out, format);
    write(out, scale);
    write(out, result.hash);
    out.write(reinterpret_cast<const char *>(pixels), bytes);
    out.close();
    if (!MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
      std::filesystem::remove(temporary);
  } catch (const std::exception &) {
    // Caching is optional; failures never prevent the original sky from baking.
  }
  return result;
}
} // namespace proxy::remix
