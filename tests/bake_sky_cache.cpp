#include "bake_sky_cache.h"
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace {
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
std::vector<uint8_t> image(const char *name) {
  std::ifstream in(name, std::ios::binary);
  require(bool(in), "Read image fixture");
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
}
int main(int argc, char **argv) {
  try {
    require(argc == 2 || argc == 4, "Usage: bake-sky-cache-test <scratch> [capture-a capture-b]");
    const auto root = std::filesystem::path(argv[1]) /
        (std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    std::filesystem::create_directories(root);
    const auto file = root / "capture.sky";
    std::vector<uint8_t> pixels(16 * 16 * 4, 100);
    for (size_t i = 3; i < pixels.size(); i += 4)
      pixels[i] = 255;
    remixapi_TextureInfo source{};
    source.width = source.height = 16;
    source.depth = source.mipLevels = 1;
    source.format = REMIXAPI_FORMAT_R8G8B8A8_SRGB;
    source.data = pixels.data();
    source.dataSize = pixels.size();
    const auto first = proxy::remix::canonicalBakeSky(source, 1, file);
    require(first.hash && !first.reused && std::filesystem::exists(file), "Store initial image");
    ++pixels[0];
    const auto noise = proxy::remix::canonicalBakeSky(source, 1, file);
    require(noise.reused && noise.hash == first.hash && noise.pixels[0] == 100,
            "Reuse actual original pixels under sparse one-level noise");
    pixels[0] = 102;
    require(!proxy::remix::canonicalBakeSky(source, 1, file).reused, "Reject larger local change");
    require(!proxy::remix::canonicalBakeSky(source, 2, file).reused, "Reject scale change");
    for (size_t i = 0; i < pixels.size(); ++i)
      if (i % 4 != 3)
        ++pixels[i];
    require(!proxy::remix::canonicalBakeSky(source, 2, file).reused, "Reject uniform one-level shift");
    pixels[3] = 254;
    require(!proxy::remix::canonicalBakeSky(source, 2, file).reused, "Reject alpha change");
    source.width = 32;
    source.height = 8;
    require(!proxy::remix::canonicalBakeSky(source, 2, file).reused, "Reject dimension change");
    {
      std::fstream corrupt(file, std::ios::binary | std::ios::in | std::ios::out);
      corrupt.seekp(-1, std::ios::end);
      corrupt.put(0);
    }
    require(!proxy::remix::canonicalBakeSky(source, 2, file).reused, "Reject corrupt payload checksum");
    require(proxy::remix::canonicalBakeSky(source, 2, file).reused, "Recover by rewriting valid capture");
    source.dataSize = 1;
    require(!proxy::remix::canonicalBakeSky(source, 2, file).hash, "Reject malformed source");
    if (argc == 4) {
      auto a = image(argv[2]), b = image(argv[3]);
      require(a.size() == 2048 * 1024 * 4 && b.size() == a.size(), "Real capture dimensions");
      source.width = 2048;
      source.height = 1024;
      source.dataSize = a.size();
      source.data = a.data();
      const auto actual = proxy::remix::canonicalBakeSky(source, 1, root / "real.sky");
      source.data = b.data();
      const auto repeated = proxy::remix::canonicalBakeSky(source, 1, root / "real.sky");
      require(repeated.reused && repeated.hash == actual.hash && repeated.pixels == a,
              "Real cross-launch captured noise reuses the exact previous transport image");
    }
    std::puts("Bake sky capture-noise reuse, sky changes, corruption and dimensions passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
