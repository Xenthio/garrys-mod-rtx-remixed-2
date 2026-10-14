#include "source_fs.h"

#include "lzma_source.h"

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
  // An entry: where its data starts, its stored size, its size, and whether
  // it is LZMA compressed (zip method 14; decoded when first read).
  struct Entry {
    uint32_t start = 0, stored = 0, size = 0;
    bool lzma = false;
  };
  std::unordered_map<std::string, Entry> entries;
  std::unordered_map<std::string, std::vector<uint8_t>> decoded;
  // An entry's bytes (nullptr: one that does not decode).
  const std::vector<uint8_t> *bytes(const std::string &key, const Entry &e) {
    auto [it, added] = decoded.try_emplace(key);
    if (added && !lzma::decodeZip(data.data() + e.start, e.stored, e.size, it->second))
      it->second.clear();
    return it->second.size() == e.size ? &it->second : nullptr;
  }
};

std::mutex g_lock;
std::wstring g_root;
std::vector<SearchPath> g_paths;
bool g_indexed = false;
Pak g_pak;

void addLoose(const std::wstring &directory) {
  g_paths.push_back({g_root + L"\\" + directory + L"\\", nullptr});
}

void addVpk(const std::wstring &dirFile) {
  auto vpk = std::make_unique<Vpk>();
  if (vpk->open(g_root + L"\\" + dirFile))
    g_paths.push_back({{}, std::move(vpk)});
}

// GMod's own file system (filesystem_stdio IBaseFileSystem, VBaseFileSystem011)
// for what the proxy's index lacks: workshop addons (steamapps/workshop/.gma,
// LZMA legacy archives), mounted games, downloads. Slots: Read 0, Open 2,
// Close 3, Seek 4, Tell 5 (Size's two overloads are not relied on).
void *engineFileSystem() {
  static void *fs = nullptr;
  if (!fs) {
    using CreateInterfaceFn = void *(*)(const char *, int *);
    HMODULE module = GetModuleHandleA("filesystem_stdio.dll");
    auto factory = module ? reinterpret_cast<CreateInterfaceFn>(GetProcAddress(module, "CreateInterface")) : nullptr;
    fs = factory ? factory("VBaseFileSystem011", nullptr) : nullptr;
  }
  return fs;
}

bool engineRead(const std::string &path, std::vector<uint8_t> *out, uint64_t offset, uint64_t length,
                uint64_t *size) {
  void *fs = engineFileSystem();
  if (!fs)
    return false;
  void **vt = *static_cast<void ***>(fs);
  using OpenFn = void *(*)(void *, const char *, const char *, const char *);
  using CloseFn = void (*)(void *, void *);
  using SeekFn = void (*)(void *, void *, long long, int);
  using TellFn = unsigned int (*)(void *, void *);
  using ReadFn = int (*)(void *, void *, int, void *);
  void *file = reinterpret_cast<OpenFn>(vt[2])(fs, path.c_str(), "rb", "GAME");
  if (!file)
    return false;
  reinterpret_cast<SeekFn>(vt[4])(fs, file, 0, 2); // FILESYSTEM_SEEK_TAIL
  uint64_t total = reinterpret_cast<TellFn>(vt[5])(fs, file);
  // A file inside a VPK the engine mounted (another game's content: Black
  // Mesa, the episodes) is left one byte short of its end by a seek to the
  // tail. Every offset counted from the end was then off by one, and the
  // texture hashes made from them matched nothing the engine uploaded.
  // What can still be read from there is the rest of the file.
  {
    uint8_t rest[16];
    const int got = reinterpret_cast<ReadFn>(vt[0])(fs, rest, int(sizeof(rest)), file);
    if (got > 0 && got < int(sizeof(rest)))
      total += uint64_t(got);
  }
  bool ok = true;
  if (size)
    *size = total;
  if (out) {
    ok = offset <= total;
    length = ok ? std::min<uint64_t>(length, total - offset) : 0;
    out->resize(size_t(length));
    reinterpret_cast<SeekFn>(vt[4])(fs, file, (long long)offset, 0); // FILESYSTEM_SEEK_HEAD
    for (uint64_t done = 0; ok && done < length;) {
      const int chunk = int(std::min<uint64_t>(length - done, 64u << 20));
      const int got = reinterpret_cast<ReadFn>(vt[0])(fs, out->data() + done, chunk, file);
      ok = got == chunk;
      done += got > 0 ? uint64_t(got) : 0;
    }
  }
  reinterpret_cast<CloseFn>(vt[3])(fs, file);
  return ok;
}

void index() {
  if (g_indexed)
    return;
  g_indexed = true;
  // garrysmod/gameinfo.txt SearchPaths (GMod x64 layout: the exe sits in the
  // game root). Loose addon folders (not .gma archives) come first.
  WIN32_FIND_DATAW found;
  HANDLE find = FindFirstFileW((g_root + L"\\garrysmod\\addons\\*").c_str(), &found);
  if (find != INVALID_HANDLE_VALUE) {
    do
      if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && found.cFileName[0] != L'.')
        addLoose(std::wstring(L"garrysmod\\addons\\") + found.cFileName);
    while (FindNextFileW(find, &found));
    FindClose(find);
  }
  addVpk(L"garrysmod\\garrysmod_dir.vpk");
  for (const wchar_t *vpk : {L"hl2_textures", L"hl2_sound_vo_english", L"hl2_sound_misc", L"hl2_misc"})
    addVpk(std::wstring(L"sourceengine\\") + vpk + L"_dir.vpk");
  addVpk(L"platform\\platform_misc_dir.vpk");
  addLoose(L"garrysmod");
  addLoose(L"sourceengine");
  addLoose(L"platform");
  addLoose(L"garrysmod\\download");
  // Mounted game content (cfg/mount.cfg) as shipped with GMod, then the
  // missing-content fallbacks.
  addVpk(L"sourceengine\\content_hl2_dir.vpk");
  addVpk(L"sourceengine\\content_cstrike_dir.vpk");
  addVpk(L"garrysmod\\fallbacks_dir.vpk");
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
    const uint32_t method = u16(at + 10), stored = u32(at + 20), size = u32(at + 24), nameLength = u16(at + 28),
                   extra = u16(at + 30), comment = u16(at + 32), local = u32(at + 42);
    if (at + 46 + nameLength > d.size())
      break;
    std::string name(reinterpret_cast<const char *>(d.data() + at + 46), nameLength);
    // Stored (0), or LZMA (14: newer maps compress their pakfile).
    if ((method == 0 || method == 14) && local + 30 <= d.size() && u32(local) == 0x04034b50) {
      const uint32_t start = local + 30 + u16(local + 26) + u16(local + 28);
      if (uint64_t(start) + stored <= d.size())
        g_pak.entries[normalize(name)] = {start, stored, method == 0 ? stored : size, method == 14};
    }
    at += 46 + nameLength + extra + comment;
  }
}

bool size(const std::string &path, uint64_t &out) {
  std::lock_guard<std::mutex> lock(g_lock);
  index();
  const std::string key = normalize(path);
  if (auto it = g_pak.entries.find(key); it != g_pak.entries.end()) {
    out = it->second.size;
    return true;
  }
  for (const auto &p : g_paths) {
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    if (!p.directory.empty() && GetFileAttributesExW((p.directory + widen(key)).c_str(), GetFileExInfoStandard, &attributes) &&
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
  return engineRead(key, nullptr, 0, 0, &out);
}

bool read(const std::string &path, std::vector<uint8_t> &out, uint64_t offset, uint64_t length) {
  std::lock_guard<std::mutex> lock(g_lock);
  index();
  const std::string key = normalize(path);
  if (auto it = g_pak.entries.find(key); it != g_pak.entries.end()) {
    const Pak::Entry &e = it->second;
    if (offset > e.size)
      return false;
    length = std::min<uint64_t>(length, e.size - offset);
    if (e.lzma) {
      const auto *bytes = g_pak.bytes(key, e);
      if (!bytes)
        return false;
      out.assign(bytes->begin() + ptrdiff_t(offset), bytes->begin() + ptrdiff_t(offset + length));
      return true;
    }
    out.assign(g_pak.data.begin() + e.start + offset, g_pak.data.begin() + e.start + offset + length);
    return true;
  }
  for (const auto &p : g_paths) {
    if (!p.directory.empty() && readFile(p.directory + widen(key), out, offset, length))
      return true;
    if (p.vpk)
      if (auto it = p.vpk->entries.find(key); it != p.vpk->entries.end())
        return p.vpk->read(it->second, out, offset, length);
  }
  return engineRead(key, &out, offset, length, nullptr);
}

// The engine's own answer for a file, whatever the index holds.
bool engineSize(const std::string &path, uint64_t &out) {
  std::lock_guard<std::mutex> lock(g_lock);
  return engineRead(normalize(path), nullptr, 0, 0, &out);
}

bool engineReadFile(const std::string &path, std::vector<uint8_t> &out, uint64_t offset, uint64_t length) {
  std::lock_guard<std::mutex> lock(g_lock);
  return engineRead(normalize(path), &out, offset, length, nullptr);
}

std::wstring locate(const std::string &path) {
  std::lock_guard<std::mutex> lock(g_lock);
  index();
  const std::wstring key = widen(normalize(path));
  for (const auto &p : g_paths)
    if (!p.directory.empty() && GetFileAttributesW((p.directory + key).c_str()) != INVALID_FILE_ATTRIBUTES)
      return p.directory + key;
  return {};
}

} // namespace proxy::fs
