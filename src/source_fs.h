#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Read-only content index: embedded map pakfile, loose addons, optional game
// mounts, then the retained GMod search paths. The engine filesystem supplies
// content absent from this index. Paths are relative ("materials/x.vmt"),
// matched case-insensitively with '/' separators.
namespace proxy::fs {

// `gameRoot`: the directory holding GMod's executable. VPK directories are
// indexed on first use and kept for the process lifetime.
// Optional OPENREMIX_PROXY_CONTENT_MOUNTS is a semicolon-separated list of
// absolute game-content directories. In list order, each contributes loose
// files before its *_dir.vpk archives (sorted by filename). All access is read-only.
void init(const std::wstring &gameRoot);

// Files of the loaded map's PAKFILE lump (a stored zip) take precedence;
// empty clears them.
void setPakfile(std::vector<uint8_t> zip);

// File size in bytes.
bool size(const std::string &path, uint64_t &out);

// Reads `length` bytes from `offset` (the rest of the file by default).
bool read(const std::string &path, std::vector<uint8_t> &out, uint64_t offset = 0, uint64_t length = ~0ull);

// The same through the engine's file system alone. A workshop addon can
// replace a stock file: the engine then has another copy than the index
// (which is asked first by size/read; a map addon shipping its own
// gm_construct/grass1.vtf was seen).
bool engineSize(const std::string &path, uint64_t &out);
bool engineReadFile(const std::string &path, std::vector<uint8_t> &out, uint64_t offset = 0, uint64_t length = ~0ull);

// Absolute path of a loose file in the search paths ("maps/x.bsp"), or empty.
std::wstring locate(const std::string &path);

} // namespace proxy::fs
