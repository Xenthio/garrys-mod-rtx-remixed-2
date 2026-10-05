#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Read-only view of the game's content as Source's file system sees it: the
// map's embedded pakfile, then each search path (gameinfo.txt order) as a
// loose directory and its pak01_dir.vpk. Paths are relative ("materials/x.vmt"),
// matched case-insensitively with '/' separators.
namespace proxy::fs {

// `gameRoot`: the directory holding left4dead2.exe. VPK directories are
// indexed on first use and kept for the process lifetime.
void init(const std::wstring &gameRoot);

// Files of the loaded map's PAKFILE lump (a stored zip) take precedence;
// empty clears them.
void setPakfile(std::vector<uint8_t> zip);

// File size in bytes.
bool size(const std::string &path, uint64_t &out);

// Reads `length` bytes from `offset` (the rest of the file by default).
bool read(const std::string &path, std::vector<uint8_t> &out, uint64_t offset = 0, uint64_t length = ~0ull);

// Absolute path of a loose file in the search paths ("maps/x.bsp"), or empty.
std::wstring locate(const std::string &path);

} // namespace proxy::fs
