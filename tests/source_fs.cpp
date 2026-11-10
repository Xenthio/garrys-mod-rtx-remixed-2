#include "source_fs.h"
#include <windows.h>
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

static void modelArchive(const std::filesystem::path &path, const char *name, const std::string &contents) {
  std::filesystem::create_directories(path.parent_path());
  // One inline model entry, no preload or numbered archive.
  std::string tree;
  const auto string = [&](const char *s) { tree.append(s); tree.push_back(0); };
  const auto number = [&](uint32_t n, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) tree.push_back(char(n >> (8 * i)));
  };
  string("mdl"); string("models"); string(name);
  number(0, 4); number(0, 2); number(0x7fff, 2);
  number(0, 4); number(uint32_t(contents.size()), 4); number(0xffff, 2);
  string(""); string(""); string("");
  {
    std::ofstream vpk(path, std::ios::binary);
    const uint32_t header[] = {0x55aa1234, 1, uint32_t(tree.size())};
    vpk.write(reinterpret_cast<const char *>(header), sizeof(header));
    vpk.write(tree.data(), std::streamsize(tree.size()));
    vpk << contents;
  }
}

int main(int argc, char **argv) {
  require(argc == 2 || (argc == 3 && std::string(argv[2]) == "without-mounts"), "Supply a scratch directory");
  const bool mountsEnabled = argc == 2;
  const auto root = std::filesystem::absolute(argv[1]);
  std::filesystem::create_directories(root / "garrysmod/models");
  std::ofstream(root / "garrysmod/models/loose.mdl", std::ios::binary) << "loose";
  modelArchive(root / "garrysmod/garrysmod_dir.vpk", "packed", "packed");
  const auto first = root / L"mounted game \u03bb", second = root / "second game";
  modelArchive(first / "game_pak_dir.vpk", "external", "external archive");
  modelArchive(first / "other_dir.vpk", "override", "archive value");
  std::filesystem::create_directories(first / "models");
  std::filesystem::create_directories(second / "models");
  std::filesystem::create_directories(root / "garrysmod/addons/custom/models");
  std::ofstream(first / "models/override.mdl") << "loose override";
  std::ofstream(first / "models/ordering.mdl") << "first root";
  std::ofstream(second / "models/ordering.mdl") << "second root";
  std::ofstream(first / "models/priority.mdl") << "mounted root";
  std::ofstream(root / "garrysmod/addons/custom/models/priority.mdl") << "addon";
  const auto mounts = (root / "missing directory").wstring() + L"; ; " + first.wstring() +
                      L";" + second.wstring() + L";" + first.wstring() + L";";
  require(SetEnvironmentVariableW(L"OPENREMIX_PROXY_CONTENT_MOUNTS", mountsEnabled ? mounts.c_str() : nullptr),
          "Configure explicit process-local content roots");
  proxy::fs::init(root.wstring());
  for (const char *name : {"models/packed.mdl", "./MODELS/packed.mdl", ".\\.\\models\\packed.mdl",
                            "models//packed.mdl", "models\\\\packed.mdl", ".//models/\\/packed.mdl"}) {
    uint64_t size = 0;
    std::vector<uint8_t> bytes;
    require(proxy::fs::size(name, size) && size == 6, "Relative model prefix must resolve VPK size");
    require(proxy::fs::read(name, bytes) && std::string(bytes.begin(), bytes.end()) == "packed",
            "Relative model prefix must resolve VPK content");
  }
  require(!proxy::fs::locate("./models/loose.mdl").empty(), "Loose lookup uses the same normalization");
  if (!mountsEnabled) {
    uint64_t size = 0;
    require(!proxy::fs::size("models/external.mdl", size), "Unset mount list retains the original content roots");
    std::puts("Default Source content roots passed without optional mounts");
    return 0;
  }
  const auto expect = [&](const char *name, const std::string &expected) {
    uint64_t size = 0;
    std::vector<uint8_t> bytes;
    require(proxy::fs::size(name, size) && size == expected.size(), "Mounted file size agrees with its selected source");
    require(proxy::fs::read(name, bytes) && std::string(bytes.begin(), bytes.end()) == expected,
            "Mounted content preserves directory and loose/archive precedence");
  };
  expect("models/external.mdl", "external archive");
  expect("models/override.mdl", "loose override");
  expect("models/ordering.mdl", "first root");
  expect("models/priority.mdl", "addon");
  const auto located = proxy::fs::locate("models/override.mdl");
  require(!located.empty() && std::filesystem::equivalent(located, first / "models/override.mdl"),
          "Loose mounted paths retain spaces and Unicode directory names");
  std::puts("Source loose/VPK paths and explicit mounted-game content passed");
}
