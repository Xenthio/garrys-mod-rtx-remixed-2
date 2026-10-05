#include "source_fs.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace proxy::fs {
namespace {

std::string normalize(std::string path) {
  for (auto &c : path)
    c = c == '\\' ? '/' : char(std::tolower(static_cast<unsigned char>(c)));
  while (!path.empty() && path[0] == '/')
    path.erase(0, 1);
  return path;
}

std::wstring widen(const std::string &s) {
  std::wstring out(s.size(), L'\0');
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), int(out.size()));
  out.resize(std::max(n, 0));
  return out;
}

bool readFile(const std::wstring &path, std::vector<uint8_t> &out, uint64_t offset, uint64_t length) {
  FILE *f = _wfopen(path.c_str(), L"rb");
  if (!f)
    return false;
  _fseeki64(f, 0, SEEK_END);
  const uint64_t total = uint64_t(_ftelli64(f));
  if (offset > total) {
    fclose(f);
    return false;
  }
  length = std::min(length, total - offset);
  out.resize(size_t(length));
  _fseeki64(f, int64_t(offset), SEEK_SET);
  const bool ok = fread(out.data(), 1, out.size(), f) == out.size();
  fclose(f);
  return ok;
}

// One pak01_dir.vpk (version 1 or 2) and its numbered archives.
struct Vpk {
  struct Entry {
    uint32_t preloadOffset; // in the directory file
    uint32_t offset, length;
    uint16_t preloadBytes, archive;
  };
  std::wstring directory; // "...\pak01_" prefix
  uint32_t dataStart = 0; // archive 0x7fff: data after the tree in the dir file
  std::unordered_map<std::string, Entry> entries;

  bool open(const std::wstring &dirFile) {
    std::vector<uint8_t> data;
    if (!readFile(dirFile, data, 0, ~0ull) || data.size() < 12)
      return false;
    uint32_t signature, version, treeSize;
    std::memcpy(&signature, data.data(), 4);
    std::memcpy(&version, data.data() + 4, 4);
    std::memcpy(&treeSize, data.data() + 8, 4);
    if (signature != 0x55aa1234 || (version != 1 && version != 2))
      return false;
    const size_t header = version == 1 ? 12 : 28;
    dataStart = uint32_t(header + treeSize);
    directory = dirFile.substr(0, dirFile.size() - std::wstring(L"dir.vpk").size());
    size_t at = header;
    const size_t end = std::min(data.size(), header + treeSize);
    auto string = [&](std::string &s) {
      const size_t start = at;
      while (at < end && data[at])
        ++at;
      if (at >= end)
        return false;
      s.assign(reinterpret_cast<const char *>(data.data() + start), at - start);
      ++at;
      return true;
    };
    std::string ext, path, name;
    while (string(ext) && !ext.empty()) {
      while (string(path) && !path.empty()) {
        while (string(name) && !name.empty()) {
          if (at + 18 > end)
            return false;
          Entry e;
          std::memcpy(&e.preloadBytes, data.data() + at + 4, 2);
          std::memcpy(&e.archive, data.data() + at + 6, 2);
          std::memcpy(&e.offset, data.data() + at + 8, 4);
          std::memcpy(&e.length, data.data() + at + 12, 4);
          e.preloadOffset = uint32_t(at + 18);
          at += 18 + e.preloadBytes;
          std::string full = path == " " ? "" : path + "/";
          full += name;
          if (ext != " ")
            full += "." + ext;
          entries.emplace(normalize(full), e);
        }
      }
    }
    return true;
  }

  bool read(const Entry &e, std::vector<uint8_t> &out, uint64_t offset, uint64_t length) const {
    const uint64_t total = uint64_t(e.preloadBytes) + e.length;
    if (offset > total)
      return false;
    length = std::min(length, total - offset);
    out.resize(size_t(length));
    size_t written = 0;
    if (offset < e.preloadBytes) {
      std::vector<uint8_t> preload;
      const uint64_t n = std::min<uint64_t>(length, e.preloadBytes - offset);
      if (!readFile(directory + L"dir.vpk", preload, e.preloadOffset + offset, n) || preload.size() != n)
        return false;
      std::memcpy(out.data(), preload.data(), size_t(n));
      written = size_t(n);
    }
    if (written < length) {
      const uint64_t inArchive = offset + written - e.preloadBytes;
      std::wstring file;
      uint64_t base = e.offset;
      if (e.archive == 0x7fff) {
        file = directory + L"dir.vpk";
        base += dataStart;
      } else {
        wchar_t index[8];
        swprintf(index, 8, L"%03u", unsigned(e.archive));
        file = directory + index + L".vpk";
      }
      std::vector<uint8_t> rest;
      if (!readFile(file, rest, base + inArchive, length - written) || rest.size() != length - written)
        return false;
      std::memcpy(out.data() + written, rest.data(), rest.size());
    }
    return true;
  }
};

struct SearchPath {
  std::wstring directory; // with trailing backslash
  std::unique_ptr<Vpk> vpk;
};

// Stored (uncompressed) zip entries: name -> (data offset, size).
struct Pak {
  std::vector<uint8_t> data;
  std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> entries;
};

std::mutex g_lock;
std::wstring g_root;
std::vector<SearchPath> g_paths;
bool g_indexed = false;
Pak g_pak;

void index() {
  if (g_indexed)
    return;
  g_indexed = true;
  // left4dead2/gameinfo.txt SearchPaths.
  for (const wchar_t *dir : {L"update", L"left4dead2_dlc3", L"left4dead2_dlc2", L"left4dead2_dlc1", L"left4dead2",
                             L"hl2"}) {
    SearchPath p;
    p.directory = g_root + L"\\" + dir + L"\\";
    auto vpk = std::make_unique<Vpk>();
    if (vpk->open(p.directory + L"pak01_dir.vpk"))
      p.vpk = std::move(vpk);
    g_paths.push_back(std::move(p));
  }
}

} // namespace

void init(const std::wstring &gameRoot) {
  std::lock_guard<std::mutex> lock(g_lock);
  g_root = gameRoot;
}

void setPakfile(std::vector<uint8_t> zip) {
  std::lock_guard<std::mutex> lock(g_lock);
  g_pak = {};
  g_pak.data = std::move(zip);
  const auto &d = g_pak.data;
  auto u16 = [&](size_t at) { uint16_t v; std::memcpy(&v, d.data() + at, 2); return uint32_t(v); };
  auto u32 = [&](size_t at) { uint32_t v; std::memcpy(&v, d.data() + at, 4); return v; };
  if (d.size() < 22)
    return;
  // End of central directory: the last "PK\5\6" (a comment may follow it).
  size_t eocd = d.size() - 22;
  while (eocd > 0 && u32(eocd) != 0x06054b50)
    --eocd;
  if (u32(eocd) != 0x06054b50)
    return;
  const uint32_t count = u16(eocd + 10);
  size_t at = u32(eocd + 16);
  for (uint32_t i = 0; i < count && at + 46 <= d.size() && u32(at) == 0x02014b50; ++i) {
    const uint32_t method = u16(at + 10), size = u32(at + 20), nameLength = u16(at + 28), extra = u16(at + 30),
                   comment = u16(at + 32), local = u32(at + 42);
    if (at + 46 + nameLength > d.size())
      break;
    std::string name(reinterpret_cast<const char *>(d.data() + at + 46), nameLength);
    if (method == 0 && local + 30 <= d.size() && u32(local) == 0x04034b50) {
      const uint32_t start = local + 30 + u16(local + 26) + u16(local + 28);
      if (uint64_t(start) + size <= d.size())
        g_pak.entries[normalize(name)] = {start, size};
    }
    at += 46 + nameLength + extra + comment;
  }
}

bool size(const std::string &path, uint64_t &out) {
  std::lock_guard<std::mutex> lock(g_lock);
  index();
  const std::string key = normalize(path);
  if (auto it = g_pak.entries.find(key); it != g_pak.entries.end()) {
    out = it->second.second;
    return true;
  }
  for (const auto &p : g_paths) {
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    if (GetFileAttributesExW((p.directory + widen(key)).c_str(), GetFileExInfoStandard, &attributes) &&
        !(attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
      out = (uint64_t(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
      return true;
    }
    if (p.vpk)
      if (auto it = p.vpk->entries.find(key); it != p.vpk->entries.end()) {
        out = uint64_t(it->second.preloadBytes) + it->second.length;
        return true;
      }
  }
  return false;
}

bool read(const std::string &path, std::vector<uint8_t> &out, uint64_t offset, uint64_t length) {
  std::lock_guard<std::mutex> lock(g_lock);
  index();
  const std::string key = normalize(path);
  if (auto it = g_pak.entries.find(key); it != g_pak.entries.end()) {
    const auto [start, total] = it->second;
    if (offset > total)
      return false;
    length = std::min<uint64_t>(length, total - offset);
    out.assign(g_pak.data.begin() + start + offset, g_pak.data.begin() + start + offset + length);
    return true;
  }
  for (const auto &p : g_paths) {
    if (readFile(p.directory + widen(key), out, offset, length))
      return true;
    if (p.vpk)
      if (auto it = p.vpk->entries.find(key); it != p.vpk->entries.end())
        return p.vpk->read(it->second, out, offset, length);
  }
  return false;
}

std::wstring locate(const std::string &path) {
  std::lock_guard<std::mutex> lock(g_lock);
  index();
  const std::wstring key = widen(normalize(path));
  for (const auto &p : g_paths)
    if (GetFileAttributesW((p.directory + key).c_str()) != INVALID_FILE_ATTRIBUTES)
      return p.directory + key;
  return {};
}

} // namespace proxy::fs
