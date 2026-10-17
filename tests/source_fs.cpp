#include "source_fs.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

static void require(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
  }
}

int main(int argc, char **argv) {
  require(argc == 2, "Supply a scratch directory");
  const auto root = std::filesystem::absolute(argv[1]);
  std::filesystem::create_directories(root / "garrysmod/models");
  std::ofstream(root / "garrysmod/models/loose.mdl", std::ios::binary) << "loose";
  // A tiny real VPK: one inline model entry, no preload or numbered archive.
  std::string tree;
  const auto string = [&](const char *s) { tree.append(s); tree.push_back(0); };
  const auto number = [&](uint32_t n, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) tree.push_back(char(n >> (8 * i)));
  };
  string("mdl"); string("models"); string("packed");
  number(0, 4); number(0, 2); number(0x7fff, 2);
  number(0, 4); number(6, 4); number(0xffff, 2);
  string(""); string(""); string("");
  {
    std::ofstream vpk(root / "garrysmod/garrysmod_dir.vpk", std::ios::binary);
    const uint32_t header[] = {0x55aa1234, 1, uint32_t(tree.size())};
    vpk.write(reinterpret_cast<const char *>(header), sizeof(header));
    vpk.write(tree.data(), std::streamsize(tree.size()));
    vpk << "packed";
  }
  proxy::fs::init(root.wstring());
  for (const char *name : {"models/packed.mdl", "./MODELS/packed.mdl", ".\\.\\models\\packed.mdl"}) {
    uint64_t size = 0;
    std::vector<uint8_t> bytes;
    require(proxy::fs::size(name, size) && size == 6, "Relative model prefix must resolve VPK size");
    require(proxy::fs::read(name, bytes) && std::string(bytes.begin(), bytes.end()) == "packed",
            "Relative model prefix must resolve VPK content");
  }
  require(!proxy::fs::locate("./models/loose.mdl").empty(), "Loose lookup uses the same normalization");
  std::puts("Source loose/VPK relative model paths passed");
}
