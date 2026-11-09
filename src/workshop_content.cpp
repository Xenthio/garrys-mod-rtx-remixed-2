#include "workshop_content.h"

#include "d3d_hooks.h"
#include "diag.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace proxy::workshop {
namespace {

constexpr char kStateHeader[] = "sourcept-workshop\t1";
constexpr char kManifestPath[] = "native/manifest.json";
constexpr char kRootPrefix[] = "native/root/";
constexpr char kModsPrefix[] = "rtx-remix/mods/";
constexpr char kAddonsPrefix[] = "garrysmod/addons/";
constexpr uint64_t kManifestLimit = uint64_t(1) << 20;
// Left free on the drive after unpacking.
constexpr uint64_t kDiskMargin = uint64_t(512) << 20;

// Bytes read or written while placing: finish() waits while this moves.
std::atomic<uint64_t> g_progress{0};

void say(const char *fmt, ...) {
  char text[900];
  va_list args;
  va_start(args, fmt);
  vsnprintf(text, sizeof(text), fmt, args);
  va_end(args);
  diag::stage("workshop: %s", text);
}

// ---------------------------------------------------------------- text, paths

std::wstring widen(const std::string &s) {
  if (s.empty())
    return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring out(size_t(std::max(n, 0)), L'\0');
  if (n > 0)
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
  return out;
}

std::string narrow(const std::wstring &s) {
  if (s.empty())
    return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
  std::string out(size_t(std::max(n, 0)), '\0');
  if (n > 0)
    WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n, nullptr, nullptr);
  return out;
}

// ASCII lower case: package paths are ASCII and compare without case.
std::string fold(std::string s) {
  for (char &c : s)
    if (c >= 'A' && c <= 'Z')
      c = char(c - 'A' + 'a');
  return s;
}

bool startsWith(const std::string &s, const std::string &prefix) { return s.compare(0, prefix.size(), prefix) == 0; }

bool endsWith(const std::string &s, const char *suffix) {
  const size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

std::vector<std::string> split(const std::string &text, char separator) {
  std::vector<std::string> fields;
  for (size_t start = 0;;) {
    const size_t end = text.find(separator, start);
    fields.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
    if (end == std::string::npos)
      return fields;
    start = end + 1;
  }
}

// `inner` is `outer` or lies below it ('/' paths, folded).
bool within(const std::string &inner, const std::string &outer) {
  return inner == outer || (inner.size() > outer.size() && startsWith(inner, outer) && inner[outer.size()] == '/');
}

std::string gib(uint64_t bytes) {
  char text[32];
  snprintf(text, sizeof(text), "%.2f GiB", double(bytes) / double(uint64_t(1) << 30));
  return text;
}

// Windows file names compare without case.
std::wstring upper(std::wstring s) {
  if (!s.empty())
    CharUpperBuffW(s.data(), DWORD(s.size()));
  return s;
}

bool samePath(const std::wstring &a, const std::wstring &b) { return upper(a) == upper(b); }

// One of the two is the other or lies below it.
bool overlaps(const std::wstring &a, const std::wstring &b) {
  const std::wstring x = upper(a), y = upper(b);
  const std::wstring &shorter = x.size() <= y.size() ? x : y, &longer = x.size() <= y.size() ? y : x;
  return longer.compare(0, shorter.size(), shorter) == 0 &&
         (longer.size() == shorter.size() || longer[shorter.size()] == L'\\');
}

// A package path ('/'-separated, relative) below a folder.
std::wstring below(const std::wstring &dir, const std::string &relative) {
  if (relative.empty())
    return dir;
  std::wstring out = dir;
  if (!out.empty() && out.back() != L'\\')
    out += L'\\';
  for (const wchar_t c : widen(relative))
    out += c == L'/' ? L'\\' : c;
  return out;
}

std::wstring parentOf(const std::wstring &path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

// The \\?\ form of an absolute path that MAX_PATH would cut.
std::wstring longPath(const std::wstring &path) {
  if (path.size() < MAX_PATH - 20 || path.compare(0, 4, L"\\\\?\\") == 0)
    return path;
  if (path.compare(0, 2, L"\\\\") == 0)
    return L"\\\\?\\UNC\\" + path.substr(2);
  return L"\\\\?\\" + path;
}

std::wstring fullPath(const std::wstring &path) {
  std::wstring full(GetFullPathNameW(path.c_str(), 0, nullptr, nullptr), L'\0');
  const DWORD n = full.empty() ? 0 : GetFullPathNameW(path.c_str(), DWORD(full.size()), full.data(), nullptr);
  if (n && n < full.size())
    full.resize(n);
  else
    full = path;
  while (full.size() > 3 && (full.back() == L'\\' || full.back() == L'/'))
    full.pop_back();
  return full;
}

template <typename T> T proc(HMODULE module, const char *name) {
  return module ? reinterpret_cast<T>(reinterpret_cast<void *>(GetProcAddress(module, name))) : nullptr;
}

// ---------------------------------------------------------------- files

struct Status {
  bool exists = false, directory = false;
  uint64_t size = 0, time = 0; // time: the last write, as a FILETIME
};

Status status(const std::wstring &path) {
  WIN32_FILE_ATTRIBUTE_DATA a;
  if (!GetFileAttributesExW(longPath(path).c_str(), GetFileExInfoStandard, &a))
    return {};
  Status s;
  s.exists = true;
  s.directory = (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  s.size = (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
  s.time = (uint64_t(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
  return s;
}

bool readFile(const std::wstring &path, std::string &out, uint64_t limit) {
  HANDLE f = CreateFileW(longPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE)
    return false;
  LARGE_INTEGER size;
  bool ok = GetFileSizeEx(f, &size) && uint64_t(size.QuadPart) <= limit;
  if (ok) {
    out.resize(size_t(size.QuadPart));
    DWORD got = 0;
    ok = out.empty() || (ReadFile(f, out.data(), DWORD(out.size()), &got, nullptr) && got == out.size());
  }
  CloseHandle(f);
  return ok;
}

bool makeDirectories(const std::wstring &dir) {
  if (dir.empty())
    return false;
  const Status s = status(dir);
  if (s.exists)
    return s.directory;
  const std::wstring parent = parentOf(dir);
  if (!parent.empty() && parent.size() < dir.size() && !makeDirectories(parent))
    return false;
  return CreateDirectoryW(longPath(dir).c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

// The name a file is written under beside its place, before it goes there.
std::wstring temporaryName(size_t index) { return L"~sourcept" + std::to_wstring(index) + L".part"; }

bool temporary(const std::wstring &name) {
  const std::string n = fold(narrow(name));
  return n.size() > 14 && startsWith(n, "~sourcept") && endsWith(n, ".part") &&
         std::all_of(n.begin() + 9, n.end() - 5, [](char c) { return c >= '0' && c <= '9'; });
}

// Removes the empty folders in `dir` and below, deepest first, then `dir` if
// that leaves it empty (true). Files and links stay, but for the proxy's own
// temporary files a start cut short left.
bool removeEmptyFolders(const std::wstring &dir) {
  bool empty = true;
  WIN32_FIND_DATAW found;
  HANDLE find = FindFirstFileW(longPath(dir + L"\\*").c_str(), &found);
  if (find != INVALID_HANDLE_VALUE) {
    do {
      const std::wstring name = found.cFileName;
      if (name == L"." || name == L"..")
        continue;
      if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        empty = removeEmptyFolders(dir + L"\\" + name) && empty;
      else if (!temporary(name) || !DeleteFileW(longPath(dir + L"\\" + name).c_str()))
        empty = false;
    } while (FindNextFileW(find, &found));
    FindClose(find);
  }
  return empty && RemoveDirectoryW(longPath(dir).c_str());
}

// Whether a file (or a link) is anywhere in `dir`; one that cannot be read
// counts as holding one.
bool holdsFiles(const std::wstring &dir) {
  WIN32_FIND_DATAW found;
  HANDLE find = FindFirstFileW(longPath(dir + L"\\*").c_str(), &found);
  if (find == INVALID_HANDLE_VALUE)
    return true;
  bool any = false;
  do {
    const std::wstring name = found.cFileName;
    if (name != L"." && name != L"..")
      any = !(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || holdsFiles(dir + L"\\" + name);
  } while (!any && FindNextFileW(find, &found));
  FindClose(find);
  return any;
}

// The directories directly in `dir` (`directories`) or its files ending in
// `suffix` (folded), by name.
std::vector<std::wstring> list(const std::wstring &dir, bool directories, const char *suffix = "") {
  std::vector<std::wstring> names;
  WIN32_FIND_DATAW found;
  HANDLE find = FindFirstFileW(longPath(dir + L"\\*").c_str(), &found);
  if (find == INVALID_HANDLE_VALUE)
    return names;
  do {
    const std::wstring name = found.cFileName;
    const bool folder = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (name == L"." || name == L".." || folder != directories)
      continue;
    if (directories || endsWith(fold(narrow(name)), suffix))
      names.push_back(name);
  } while (FindNextFileW(find, &found));
  FindClose(find);
  std::sort(names.begin(), names.end());
  return names;
}

// ---------------------------------------------------------------- SHA-256 (CNG)

struct Bcrypt {
  using Open = LONG(WINAPI *)(void **, LPCWSTR, LPCWSTR, ULONG);
  using Create = LONG(WINAPI *)(void *, void **, PUCHAR, ULONG, PUCHAR, ULONG, ULONG);
  using Data = LONG(WINAPI *)(void *, PUCHAR, ULONG, ULONG);
  using Finish = LONG(WINAPI *)(void *, PUCHAR, ULONG, ULONG);
  using Destroy = LONG(WINAPI *)(void *);
  void *algorithm = nullptr;
  Create create = nullptr;
  Data data = nullptr;
  Finish finish = nullptr;
  Destroy destroy = nullptr;
  Bcrypt() {
    const HMODULE module = LoadLibraryExW(L"bcrypt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto open = proc<Open>(module, "BCryptOpenAlgorithmProvider");
    create = proc<Create>(module, "BCryptCreateHash");
    data = proc<Data>(module, "BCryptHashData");
    finish = proc<Finish>(module, "BCryptFinishHash");
    destroy = proc<Destroy>(module, "BCryptDestroyHash");
    if (!open || !create || !data || !finish || !destroy || open(&algorithm, L"SHA256", nullptr, 0) < 0)
      algorithm = nullptr;
  }
};

const Bcrypt &bcrypt() {
  static const Bcrypt api;
  return api;
}

class Sha256 {
public:
  Sha256() {
    const Bcrypt &api = bcrypt();
    if (!api.algorithm || api.create(api.algorithm, &hash_, nullptr, 0, nullptr, 0, 0) < 0)
      hash_ = nullptr;
  }
  ~Sha256() {
    if (hash_)
      bcrypt().destroy(hash_);
  }
  Sha256(const Sha256 &) = delete;
  Sha256 &operator=(const Sha256 &) = delete;
  bool ok() const { return hash_ && !failed_; }
  void update(const void *data, size_t size) {
    auto *bytes = static_cast<PUCHAR>(const_cast<void *>(data));
    while (ok() && size) {
      const ULONG n = ULONG(std::min<size_t>(size, size_t(1) << 30));
      failed_ = bcrypt().data(hash_, bytes, n, 0) < 0;
      bytes += n;
      size -= n;
    }
  }
  // The digest in lower-case hex (empty on failure); finishes the hash.
  std::string hex() {
    unsigned char digest[32];
    if (!ok() || bcrypt().finish(hash_, digest, sizeof(digest), 0) < 0)
      return {};
    failed_ = true;
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    for (const unsigned char b : digest) {
      out += kDigits[b >> 4];
      out += kDigits[b & 15];
    }
    return out;
  }

private:
  void *hash_ = nullptr;
  bool failed_ = false;
};

std::string hashFile(const std::wstring &path) {
  HANDLE f = CreateFileW(longPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                         FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (f == INVALID_HANDLE_VALUE)
    return {};
  Sha256 sha;
  std::vector<uint8_t> buffer(size_t(1) << 20);
  bool ok = sha.ok();
  for (DWORD got = 0; ok; sha.update(buffer.data(), got)) {
    ok = ReadFile(f, buffer.data(), DWORD(buffer.size()), &got, nullptr) != 0;
    if (!got)
      break;
    g_progress += got;
  }
  CloseHandle(f);
  return ok ? sha.hex() : std::string();
}

bool lowerHex64(const std::string &s) {
  return s.size() == 64 &&
         std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// ---------------------------------------------------------------- Valve KeyValues text (.acf, .vdf)

struct Vdf {
  std::string key, value;
  bool block = false;
  std::vector<Vdf> children;
  const Vdf *find(const std::string &name) const {
    const std::string folded = fold(name);
    for (const Vdf &child : children)
      if (fold(child.key) == folded)
        return &child;
    return nullptr;
  }
};

class VdfParser {
public:
  explicit VdfParser(const std::string &text) : s_(text) {}
  bool parse(Vdf &root) {
    root.block = true;
    return body(root, 0);
  }

private:
  const std::string &s_;
  size_t at_ = 0;

  void skip() {
    for (;;) {
      while (at_ < s_.size() && static_cast<unsigned char>(s_[at_]) <= ' ')
        ++at_;
      if (at_ + 1 < s_.size() && s_[at_] == '/' && s_[at_ + 1] == '/') {
        while (at_ < s_.size() && s_[at_] != '\n')
          ++at_;
        continue;
      }
      return;
    }
  }
  // A quoted (with \\ \" \n \t escapes, as Steam writes paths) or bare token.
  bool token(std::string &out) {
    skip();
    out.clear();
    if (at_ >= s_.size() || s_[at_] == '{' || s_[at_] == '}')
      return false;
    if (s_[at_] == '"') {
      for (++at_; at_ < s_.size() && s_[at_] != '"';) {
        char c = s_[at_++];
        if (c == '\\' && at_ < s_.size()) {
          const char e = s_[at_++];
          c = e == 'n' ? '\n' : e == 't' ? '\t' : e;
        }
        out += c;
      }
      if (at_ >= s_.size())
        return false;
      ++at_;
      return true;
    }
    while (at_ < s_.size() && static_cast<unsigned char>(s_[at_]) > ' ' && s_[at_] != '{' && s_[at_] != '}' &&
           s_[at_] != '"')
      out += s_[at_++];
    return true;
  }
  bool body(Vdf &node, int depth) {
    if (depth > 32)
      return false;
    for (;;) {
      skip();
      if (at_ >= s_.size())
        return depth == 0;
      if (s_[at_] == '}') {
        ++at_;
        return depth > 0;
      }
      Vdf child;
      if (!token(child.key))
        return false;
      skip();
      if (at_ < s_.size() && s_[at_] == '{') {
        ++at_;
        child.block = true;
        if (!body(child, depth + 1))
          return false;
      } else if (!token(child.value)) {
        return false;
      }
      // A platform conditional ("[$WIN32]") is not Steam's and is ignored.
      skip();
      if (at_ < s_.size() && s_[at_] == '[') {
        while (at_ < s_.size() && s_[at_] != ']')
          ++at_;
        at_ += at_ < s_.size();
      }
      node.children.push_back(std::move(child));
    }
  }
};

bool loadVdf(const std::wstring &path, Vdf &root) {
  std::string text;
  return readFile(path, text, uint64_t(64) << 20) && VdfParser(text).parse(root);
}

// ---------------------------------------------------------------- JSON (RFC 8259, strict)

struct Json {
  enum Type { Null, Bool, Number, String, Array, Object } type = Null;
  bool boolean = false;
  std::string text; // a string, or a number as written
  std::vector<Json> items;
  std::vector<std::pair<std::string, Json>> members;
  const Json *get(const char *key) const {
    for (const auto &member : members)
      if (member.first == key)
        return &member.second;
    return nullptr;
  }
  // A plain non-negative integer (no sign, fraction or exponent) up to 2^53.
  bool integer(uint64_t &out) const {
    if (type != Number || text.empty() || text.size() > 16)
      return false;
    out = 0;
    for (const char c : text) {
      if (c < '0' || c > '9')
        return false;
      out = out * 10 + uint64_t(c - '0');
    }
    return out <= (uint64_t(1) << 53);
  }
};

class JsonParser {
public:
  explicit JsonParser(const std::string &text) : s_(text) {}
  bool parse(Json &out) {
    if (!value(out, 0))
      return false;
    space();
    return at_ == s_.size();
  }

private:
  const std::string &s_;
  size_t at_ = 0;

  static bool digit(char c) { return c >= '0' && c <= '9'; }
  bool more() const { return at_ < s_.size(); }
  void space() {
    while (more() && (s_[at_] == ' ' || s_[at_] == '\t' || s_[at_] == '\n' || s_[at_] == '\r'))
      ++at_;
  }
  bool literal(const char *word) {
    const size_t n = std::strlen(word);
    if (s_.compare(at_, n, word) != 0)
      return false;
    at_ += n;
    return true;
  }
  bool value(Json &out, int depth) {
    space();
    if (!more() || depth > 64)
      return false;
    switch (s_[at_]) {
    case '{': return object(out, depth);
    case '[': return array(out, depth);
    case '"': out.type = Json::String; return string(out.text);
    case 't': out.type = Json::Bool; out.boolean = true; return literal("true");
    case 'f': out.type = Json::Bool; return literal("false");
    case 'n': return literal("null");
    default: return number(out);
    }
  }
  bool object(Json &out, int depth) {
    out.type = Json::Object;
    ++at_;
    space();
    if (more() && s_[at_] == '}') {
      ++at_;
      return true;
    }
    std::set<std::string> keys;
    for (;;) {
      space();
      std::string key;
      if (!more() || s_[at_] != '"' || !string(key) || !keys.insert(key).second) // no duplicate keys
        return false;
      space();
      if (!more() || s_[at_] != ':')
        return false;
      ++at_;
      Json v;
      if (!value(v, depth + 1))
        return false;
      out.members.emplace_back(std::move(key), std::move(v));
      space();
      if (more() && s_[at_] == ',') {
        ++at_;
        continue;
      }
      if (more() && s_[at_] == '}') {
        ++at_;
        return true;
      }
      return false;
    }
  }
  bool array(Json &out, int depth) {
    out.type = Json::Array;
    ++at_;
    space();
    if (more() && s_[at_] == ']') {
      ++at_;
      return true;
    }
    for (;;) {
      Json v;
      if (!value(v, depth + 1))
        return false;
      out.items.push_back(std::move(v));
      space();
      if (more() && s_[at_] == ',') {
        ++at_;
        continue;
      }
      if (more() && s_[at_] == ']') {
        ++at_;
        return true;
      }
      return false;
    }
  }
  bool digits() {
    if (!more() || !digit(s_[at_]))
      return false;
    while (more() && digit(s_[at_]))
      ++at_;
    return true;
  }
  bool number(Json &out) {
    const size_t start = at_;
    if (more() && s_[at_] == '-')
      ++at_;
    if (more() && s_[at_] == '0')
      ++at_;
    else if (!digits())
      return false;
    if (more() && s_[at_] == '.') {
      ++at_;
      if (!digits())
        return false;
    }
    if (more() && (s_[at_] == 'e' || s_[at_] == 'E')) {
      ++at_;
      if (more() && (s_[at_] == '+' || s_[at_] == '-'))
        ++at_;
      if (!digits())
        return false;
    }
    out.type = Json::Number;
    out.text = s_.substr(start, at_ - start);
    return true;
  }
  bool hex4(uint32_t &out) {
    if (at_ + 4 > s_.size())
      return false;
    out = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = s_[at_++];
      out <<= 4;
      if (digit(c))
        out |= uint32_t(c - '0');
      else if (c >= 'a' && c <= 'f')
        out |= uint32_t(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        out |= uint32_t(c - 'A' + 10);
      else
        return false;
    }
    return true;
  }
  static void utf8(std::string &out, uint32_t cp) {
    if (cp < 0x80) {
      out += char(cp);
    } else if (cp < 0x800) {
      out += char(0xC0 | (cp >> 6));
      out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += char(0xE0 | (cp >> 12));
      out += char(0x80 | ((cp >> 6) & 0x3F));
      out += char(0x80 | (cp & 0x3F));
    } else {
      out += char(0xF0 | (cp >> 18));
      out += char(0x80 | ((cp >> 12) & 0x3F));
      out += char(0x80 | ((cp >> 6) & 0x3F));
      out += char(0x80 | (cp & 0x3F));
    }
  }
  bool string(std::string &out) {
    out.clear();
    for (++at_; more();) {
      const unsigned char c = static_cast<unsigned char>(s_[at_++]);
      if (c == '"')
        return true;
      if (c < 0x20)
        return false;
      if (c == '\\') {
        if (!more())
          return false;
        switch (s_[at_++]) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          uint32_t cp = 0, low = 0;
          if (!hex4(cp) || (cp >= 0xDC00 && cp <= 0xDFFF))
            return false;
          if (cp >= 0xD800 && cp < 0xDC00) {
            if (at_ + 2 > s_.size() || s_[at_] != '\\' || s_[at_ + 1] != 'u')
              return false;
            at_ += 2;
            if (!hex4(low) || low < 0xDC00 || low > 0xDFFF)
              return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          }
          utf8(out, cp);
          break;
        }
        default: return false;
        }
        continue;
      }
      out += char(c);
      if (c >= 0x80) { // the rest of a UTF-8 sequence
        const int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC2 ? 1 : -1;
        if (extra < 0 || c > 0xF4)
          return false;
        for (int i = 0; i < extra; ++i) {
          if (!more() || (static_cast<unsigned char>(s_[at_]) & 0xC0) != 0x80)
            return false;
          out += s_[at_++];
        }
      }
    }
    return false;
  }
};

// ---------------------------------------------------------------- package rules (the native loader's)

// Why a package path is refused (nullptr: it is fine), as nl::paths::problem
// rules it, ASCII as the native loader requires of the paths it deploys.
const char *pathProblem(const std::string &path) {
  if (path.empty())
    return "an empty path";
  if (path.size() > 1024)
    return "a path longer than 1024 bytes";
  for (const char ch : path) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (c < 0x20 || c == 0x7f)
      return "a control character";
    if (c >= 0x80)
      return "a character outside ASCII";
    if (std::strchr("\\:<>\"|?*", ch))
      return "a character Windows refuses in names";
  }
  for (size_t start = 0;;) {
    const size_t slash = path.find('/', start);
    const std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    if (part.empty())
      return "an empty component";
    if (part == "." || part == "..")
      return "a . or .. component";
    if (part.size() > 255)
      return "a component longer than 255 bytes";
    if (part.front() == ' ' || part.back() == ' ' || part.back() == '.')
      return "a component starting with a space or ending with a space or dot";
    std::string base = fold(part.substr(0, part.find('.')));
    while (!base.empty() && base.back() == ' ')
      base.pop_back();
    if (base == "con" || base == "prn" || base == "aux" || base == "nul" || base == "conin$" || base == "conout$" ||
        (base.size() == 4 && (startsWith(base, "com") || startsWith(base, "lpt")) && base[3] >= '0' && base[3] <= '9'))
      return "a Windows device name";
    for (size_t i = 0; i + 1 < part.size(); ++i)
      if (part[i] == '~' && part[i + 1] >= '0' && part[i + 1] <= '9')
        return "a short (8.3) file name";
    if (slash == std::string::npos)
      return nullptr;
    start = slash + 1;
  }
}

// gmad's wildcard table (what GMod's Workshop takes in an add-on); a leading
// '!' is an exception that wins over every pattern.
constexpr const char *kWhitelist[] = {
    "lua/*.lua", "scenes/*.vcd", "particles/*.pcf", "resource/fonts/*.ttf", "scripts/vehicles/*.txt",
    "resource/localization/*/*.properties", "maps/*.bsp", "maps/*.lmp", "maps/*.nav", "maps/*.ain",
    "maps/thumb/*.png", "sound/*.wav", "sound/*.mp3", "sound/*.ogg", "materials/*.vmt", "materials/*.vtf",
    "materials/*.png", "materials/*.jpg", "materials/*.jpeg", "materials/colorcorrection/*.raw", "models/*.mdl",
    "models/*.phy", "models/*.ani", "models/*.vvd", "models/*.vtx", "!models/*.sw.vtx", "!models/*.360.vtx",
    "!models/*.xbox.vtx", "gamemodes/*/*.txt", "!gamemodes/*/*/*.txt", "gamemodes/*/*.fgd", "!gamemodes/*/*/*.fgd",
    "gamemodes/*/logo.png", "gamemodes/*/icon24.png", "gamemodes/*/gamemode/*.lua",
    "gamemodes/*/entities/effects/*.lua", "gamemodes/*/entities/weapons/*.lua", "gamemodes/*/entities/entities/*.lua",
    "gamemodes/*/backgrounds/*.png", "gamemodes/*/backgrounds/*.jpg", "gamemodes/*/backgrounds/*.jpeg",
    "gamemodes/*/content/models/*.mdl", "gamemodes/*/content/models/*.phy", "gamemodes/*/content/models/*.ani",
    "gamemodes/*/content/models/*.vvd", "gamemodes/*/content/models/*.vtx", "!gamemodes/*/content/models/*.sw.vtx",
    "!gamemodes/*/content/models/*.360.vtx", "!gamemodes/*/content/models/*.xbox.vtx",
    "gamemodes/*/content/materials/*.vmt", "gamemodes/*/content/materials/*.vtf",
    "gamemodes/*/content/materials/*.png", "gamemodes/*/content/materials/*.jpg",
    "gamemodes/*/content/materials/*.jpeg", "gamemodes/*/content/materials/colorcorrection/*.raw",
    "gamemodes/*/content/scenes/*.vcd", "gamemodes/*/content/particles/*.pcf",
    "gamemodes/*/content/resource/fonts/*.ttf", "gamemodes/*/content/scripts/vehicles/*.txt",
    "gamemodes/*/content/resource/localization/*/*.properties", "gamemodes/*/content/maps/*.bsp",
    "gamemodes/*/content/maps/*.nav", "gamemodes/*/content/maps/*.ain", "gamemodes/*/content/maps/thumb/*.png",
    "gamemodes/*/content/sound/*.wav", "gamemodes/*/content/sound/*.mp3", "gamemodes/*/content/sound/*.ogg",
    "data_static/*.txt", "data_static/*.dat", "data_static/*.json", "data_static/*.xml", "data_static/*.csv",
    "shaders/fxc/*.vcs",
};

// Source's wildcard match: '*' matches anything, '/' included; '?' one character.
bool wildcard(const char *pattern, const std::string &text) {
  const size_t n = std::strlen(pattern);
  size_t p = 0, i = 0, star = std::string::npos, mark = 0;
  while (i < text.size()) {
    if (p < n && pattern[p] == '*') {
      star = p++;
      mark = i;
    } else if (p < n && (pattern[p] == text[i] || pattern[p] == '?')) {
      ++p;
      ++i;
    } else if (star != std::string::npos) {
      p = star + 1;
      i = ++mark;
    } else {
      return false;
    }
  }
  while (p < n && pattern[p] == '*')
    ++p;
  return p == n;
}

bool whitelisted(const std::string &folded) {
  bool allowed = false;
  for (const char *pattern : kWhitelist) {
    if (pattern[0] == '!') {
      if (wildcard(pattern + 1, folded))
        return false;
    } else if (wildcard(pattern, folded)) {
      allowed = true;
    }
  }
  return allowed;
}

bool remixModFile(const std::string &folded) {
  for (const char *type : {".usd", ".usda", ".usdc", ".usdz", ".dds", ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".exr",
                           ".hdr", ".mdl", ".json", ".txt"})
    if (endsWith(folded, type))
      return true;
  return false;
}

// ---------------------------------------------------------------- the archive (GMAD 1-3)

struct Member {
  std::string name;
  uint64_t size = 0, offset = 0;
};

struct Archive {
  std::vector<Member> members;
  std::map<std::string, size_t> byName; // folded
};

class Reader {
public:
  explicit Reader(HANDLE file) : file_(file) {}
  bool bytes(void *out, size_t n) {
    auto *p = static_cast<uint8_t *>(out);
    while (n) {
      if (at_ == size_ && !fill())
        return false;
      const size_t k = std::min(n, size_ - at_);
      std::memcpy(p, buffer_.data() + at_, k);
      at_ += k;
      p += k;
      n -= k;
    }
    return true;
  }
  // A NUL-terminated string of at most `limit` bytes.
  bool string(std::string &out, size_t limit) {
    out.clear();
    for (;;) {
      if (at_ == size_ && !fill())
        return false;
      const uint8_t *begin = buffer_.data() + at_, *end = buffer_.data() + size_;
      const auto *zero = static_cast<const uint8_t *>(std::memchr(begin, 0, size_t(end - begin)));
      const size_t k = size_t((zero ? zero : end) - begin);
      if (out.size() + k > limit)
        return false;
      out.append(reinterpret_cast<const char *>(begin), k);
      at_ += k;
      if (zero) {
        ++at_;
        return true;
      }
    }
  }
  uint64_t position() const { return consumed_ + at_; }
  // A read failed (as opposed to the file ending): worth trying again later.
  bool failed() const { return failed_; }

private:
  bool fill() {
    consumed_ += size_;
    at_ = size_ = 0;
    DWORD got = 0;
    if (!ReadFile(file_, buffer_.data(), DWORD(buffer_.size()), &got, nullptr)) {
      failed_ = true;
      return false;
    }
    size_ = got;
    return got != 0;
  }
  HANDLE file_;
  std::vector<uint8_t> buffer_ = std::vector<uint8_t>(size_t(1) << 16);
  size_t at_ = 0, size_ = 0;
  uint64_t consumed_ = 0;
  bool failed_ = false;
};

// The index of a GMod addon archive (GMAD 1-3, as gmad writes it). `io`: the
// failure was a read error, not the archive's contents.
bool readArchive(HANDLE f, Archive &a, std::string &why, bool &io) {
  io = false;
  LARGE_INTEGER total;
  if (!GetFileSizeEx(f, &total)) {
    why = "its size could not be read (error " + std::to_string(GetLastError()) + ")";
    io = true;
    return false;
  }
  Reader r(f);
  char magic[4];
  uint8_t version = 0;
  uint64_t steamId = 0, stamp = 0;
  int32_t addonVersion = 0;
  std::string text;
  if (!r.bytes(magic, 4) || std::memcmp(magic, "GMAD", 4) != 0 || !r.bytes(&version, 1) || version < 1 || version > 3) {
    io = r.failed();
    why = io ? "it could not be read" : "it is not a GMAD archive";
    return false;
  }
  bool ok = r.bytes(&steamId, 8) && r.bytes(&stamp, 8);
  while (ok && version > 1 && (ok = r.string(text, 4096)) && !text.empty()) {
  } // required content
  for (int i = 0; ok && i < 3; ++i) // name, description, author
    ok = r.string(text, size_t(1) << 20);
  ok = ok && r.bytes(&addonVersion, 4);
  for (uint32_t number = 1; ok && (ok = r.bytes(&number, 4)) && number;) {
    Member m;
    int64_t length = 0;
    uint32_t crc = 0;
    ok = r.string(m.name, 4096) && r.bytes(&length, 8) && r.bytes(&crc, 4) && length >= 0 &&
         a.members.size() < 1000000;
    m.size = uint64_t(length);
    a.members.push_back(std::move(m));
  }
  if (!ok) {
    io = r.failed();
    why = io ? "its index could not be read" : "its index is damaged";
    return false;
  }
  uint64_t offset = r.position();
  for (size_t i = 0; i < a.members.size(); ++i) {
    Member &m = a.members[i];
    if (offset > uint64_t(total.QuadPart) || m.size > uint64_t(total.QuadPart) - offset) {
      why = "it is shorter than its index";
      return false;
    }
    m.offset = offset;
    offset += m.size;
    if (!a.byName.emplace(fold(m.name), i).second) {
      why = "it holds " + m.name + " twice";
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------- the package

struct File {
  std::string path; // below its root ('/'); empty when the root is this file
  uint64_t size = 0;
  std::string sha256;
  size_t member = 0;
};

struct Root {
  std::string path; // below the game root: rtx-remix/mods/<mod>... or garrysmod/addons/<add-on>...
  bool addon = false, createOnly = false;
  std::vector<File> files;
};

struct Package {
  std::string item, id, version, manifest; // manifest: the SHA-256 of manifest.json
  bool requiresRemix = false;              // its add-ons only make sense with a runtime
  std::wstring archivePath;
  Archive archive;
  std::vector<Root> roots;
};

// The native loader version whose content-package rules this follows: a
// package that requires a newer loader may rely on rules it does not know.
constexpr uint64_t kLoaderRules[3] = {0, 16, 0};

// MAJOR.MINOR.PATCH of a SemVer 2.0.0 version (true: `text` is one).
bool semver(const std::string &text, uint64_t core[3]) {
  const auto numeric = [](const std::string &s) {
    return !s.empty() && s.size() <= 18 && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; }) &&
           (s.size() == 1 || s[0] != '0');
  };
  const auto identifiers = [&](const std::string &s, bool pre) {
    for (const std::string &part : split(s, '.'))
      if (part.empty() ||
          !std::all_of(part.begin(), part.end(),
                       [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-'; }) ||
          (pre && std::all_of(part.begin(), part.end(), [](char c) { return c >= '0' && c <= '9'; }) && !numeric(part)))
        return false;
    return true;
  };
  const size_t plus = text.find('+'), dash = text.substr(0, plus).find('-');
  const auto parts = split(text.substr(0, std::min(dash, plus)), '.');
  if (parts.size() != 3 || !std::all_of(parts.begin(), parts.end(), numeric))
    return false;
  for (int i = 0; i < 3; ++i)
    core[i] = std::strtoull(parts[size_t(i)].c_str(), nullptr, 10);
  if (dash != std::string::npos && !identifiers(text.substr(dash + 1, plus == std::string::npos ? plus : plus - dash - 1), true))
    return false;
  return plus == std::string::npos || identifiers(text.substr(plus + 1), false);
}

// Why the manifest is not a content-only package this proxy can place (empty:
// it is one), filling `p`. The rules are the native loader's for packages
// that need no allowlist (content.cpp, manifest.cpp).
std::string validate(const Json &manifest, Package &p) {
  if (manifest.type != Json::Object)
    return "its manifest is not a JSON object";
  static const std::set<std::string> kNativeOnly = {"remix",         "reshade",        "patch_sets",
                                                    "module_overrides", "session_cvars", "module_dependencies"};
  static const std::set<std::string> kKnown = {"schema", "id",          "name",           "version",   "kinds",
                                               "files",  "root_files", "requires_loader", "requires_remix"};
  for (const auto &member : manifest.members) {
    if (kNativeOnly.count(member.first))
      return "it has \"" + member.first + "\", which only the GMod native loader applies";
    if (!kKnown.count(member.first))
      return "its manifest has an unknown key \"" + member.first + "\"";
  }
  const Json *schema = manifest.get("schema"), *id = manifest.get("id"), *version = manifest.get("version");
  if (!schema || schema->type != Json::Number || schema->text != "1")
    return "it is not schema 1";
  if (!id || id->type != Json::String || id->text.empty() || id->text.size() > 64 || id->text[0] == '-' ||
      !std::all_of(id->text.begin(), id->text.end(),
                   [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; }))
    return "its id is not 1-64 of a-z 0-9 -";
  uint64_t core[3] = {};
  if (!version || version->type != Json::String || version->text.size() > 256 || !semver(version->text, core))
    return "its version is not a SemVer 2.0.0 version";
  p.id = id->text;
  p.version = version->text;
  const Json *name = manifest.get("name"), *loader = manifest.get("requires_loader"),
             *remix = manifest.get("requires_remix");
  if (name && (name->type != Json::String || name->text.empty() || name->text.size() > 128))
    return "its name is not 1-128 bytes";
  if (remix && remix->type != Json::Bool)
    return "its requires_remix is not true or false";
  p.requiresRemix = remix && remix->boolean;
  if (loader) {
    uint64_t needed[3] = {};
    if (loader->type != Json::String || loader->text.compare(0, 2, ">=") != 0 ||
        loader->text.find_first_of("-+") != std::string::npos || !semver(loader->text.substr(2), needed))
      return "its requires_loader is not \">=MAJOR.MINOR.PATCH\"";
    if (std::lexicographical_compare(kLoaderRules, kLoaderRules + 3, needed, needed + 3))
      return "it requires loader " + loader->text.substr(2) + ", newer than the rules this follows";
  }

  const Json *kinds = manifest.get("kinds");
  if (!kinds || kinds->type != Json::Array || kinds->items.empty())
    return "it names no kinds";
  std::set<std::string> kindSet;
  for (const Json &kind : kinds->items) {
    if (kind.type != Json::String || !kindSet.insert(kind.text).second)
      return "its kinds are not distinct strings";
    if (kind.text != "root-files" && kind.text != "lua-addon")
      return "it is a \"" + kind.text + "\" package, which only the GMod native loader deploys";
  }

  const Json *roots = manifest.get("root_files");
  if (!roots || roots->type != Json::Array || roots->items.empty())
    return "it places no root files";
  for (const Json &entry : roots->items) {
    if (entry.type != Json::Object)
      return "a root file is not an object";
    for (const auto &member : entry.members)
      if (member.first != "path" && member.first != "policy")
        return "a root file has an unknown key \"" + member.first + "\"";
    const Json *path = entry.get("path"), *policy = entry.get("policy");
    if (!path || path->type != Json::String)
      return "a root file has no path";
    if (const char *problem = pathProblem(path->text))
      return "root file " + path->text + " has " + problem;
    if (policy && (policy->type != Json::String || (policy->text != "replace-owned" && policy->text != "create-only")))
      return "root file " + path->text + " has an unknown policy";
    Root root;
    root.path = path->text;
    root.createOnly = policy && policy->text == "create-only";
    const std::string folded = fold(path->text);
    // Every file inside a named folder: rtx-remix/mods/<mod>/..., garrysmod/addons/<add-on>/...
    if (startsWith(folded, kModsPrefix) && folded.size() > std::strlen(kModsPrefix)) {
      if (!kindSet.count("root-files"))
        return "it places " + root.path + " without the kind root-files";
    } else if (startsWith(folded, kAddonsPrefix) && folded.size() > std::strlen(kAddonsPrefix)) {
      if (!kindSet.count("lua-addon"))
        return "it places " + root.path + " without the kind lua-addon";
      if (root.createOnly)
        return "add-on folder " + root.path + " is create-only (they are replace-owned)";
      root.addon = true;
    } else {
      return "it places " + root.path + ", outside rtx-remix/mods and garrysmod/addons, as only the GMod native "
             "loader may";
    }
    for (const Root &other : p.roots)
      if (within(folded, fold(other.path)) || within(fold(other.path), folded))
        return "root files " + other.path + " and " + root.path + " overlap";
    p.roots.push_back(std::move(root));
  }

  const Json *files = manifest.get("files");
  if (!files || files->type != Json::Object)
    return "it lists no files";
  std::set<std::string> listed;
  for (const auto &[key, spec] : files->members) {
    if (const char *problem = pathProblem(key))
      return "file " + key + " has " + problem;
    const std::string folded = fold(key);
    if (!listed.insert(folded).second)
      return "it lists " + key + " twice";
    if (!startsWith(folded, kRootPrefix))
      return "it holds " + key + ", outside native/root, which only the GMod native loader deploys";
    uint64_t size = 0;
    const Json *sizeValue = spec.get("size"), *hash = spec.get("sha256");
    if (spec.type != Json::Object || spec.members.size() != 2 || !sizeValue || !sizeValue->integer(size) || !hash ||
        hash->type != Json::String || !lowerHex64(hash->text))
      return "file " + key + " has no size and SHA-256";
    const std::string deployed = key.substr(std::strlen(kRootPrefix)), area = fold(deployed);
    // Inside a named folder: rtx-remix/mods/<mod>/<file>, garrysmod/addons/<add-on>/<file>.
    if (std::count(area.begin(), area.end(), '/') < 3)
      return "file " + deployed + " is not inside a folder of rtx-remix/mods or garrysmod/addons";
    Root *owner = nullptr;
    for (Root &root : p.roots)
      if (within(area, fold(root.path))) {
        owner = &root;
        break;
      }
    if (!owner)
      return "file " + key + " lies outside every root file";
    if (owner->addon) {
      const std::string rest = area.substr(std::strlen(kAddonsPrefix));
      const size_t slash = rest.find('/');
      const std::string inner = slash == std::string::npos ? std::string() : rest.substr(slash + 1);
      if (inner != "addon.json" && !whitelisted(inner))
        return "add-on file " + deployed + " is not one GMod's Workshop allows";
    } else if (!remixModFile(area)) {
      return "file " + deployed + " is not Remix mod data";
    }
    const auto member = p.archive.byName.find(folded);
    if (member == p.archive.byName.end())
      return "file " + key + " is missing from the archive";
    if (p.archive.members[member->second].size != size)
      return "file " + key + " differs in size from the manifest";
    File file;
    file.path = deployed.size() > owner->path.size() ? deployed.substr(owner->path.size() + 1) : std::string();
    file.size = size;
    file.sha256 = hash->text;
    file.member = member->second;
    owner->files.push_back(std::move(file));
  }
  for (const Root &root : p.roots)
    if (root.files.empty())
      return "root file " + root.path + " has no files";
  for (const std::string &kind : kindSet)
    if (std::none_of(p.roots.begin(), p.roots.end(), [&](const Root &r) { return r.addon == (kind == "lua-addon"); }))
      return "it declares the kind " + kind + ", which no root file uses";
  // Nothing native rides along unlisted.
  for (const Member &m : p.archive.members) {
    const std::string folded = fold(m.name);
    if (startsWith(folded, "native/") && folded != kManifestPath && !listed.count(folded))
      return "its archive holds " + m.name + ", which the manifest does not list";
  }
  return {};
}

// Unreadable: not an archive this reads (until it changes); Invalid: a
// package that cannot be placed, or an archive that could not be read now.
enum class Read { NotPackage, Unreadable, Invalid, Valid };

Read readPackage(const std::string &item, const std::wstring &path, Package &p, std::string &why) {
  HANDLE f = CreateFileW(longPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    why = "its archive could not be opened (error " + std::to_string(GetLastError()) + ")";
    return Read::Invalid;
  }
  p.item = item;
  p.archivePath = path;
  std::string manifest;
  bool io = false;
  if (!readArchive(f, p.archive, why, io)) {
    CloseHandle(f);
    return io ? Read::Invalid : Read::Unreadable;
  }
  const auto found = p.archive.byName.find(kManifestPath);
  if (found == p.archive.byName.end()) {
    CloseHandle(f);
    return Read::NotPackage;
  }
  const Member &m = p.archive.members[found->second];
  bool ok = m.size <= kManifestLimit;
  if (!ok) {
    why = "its manifest is larger than 1 MiB";
  } else {
    LARGE_INTEGER at;
    at.QuadPart = LONGLONG(m.offset);
    DWORD got = 0;
    manifest.resize(size_t(m.size));
    ok = SetFilePointerEx(f, at, nullptr, FILE_BEGIN) &&
         (manifest.empty() ||
          (ReadFile(f, manifest.data(), DWORD(manifest.size()), &got, nullptr) && got == manifest.size()));
    if (!ok)
      why = "its manifest could not be read";
  }
  CloseHandle(f);
  if (!ok)
    return Read::Invalid;
  Sha256 sha;
  sha.update(manifest.data(), manifest.size());
  p.manifest = sha.hex();
  Json json;
  if (p.manifest.empty()) {
    why = "SHA-256 is not available (bcrypt.dll)";
    return Read::Invalid;
  }
  if (!JsonParser(manifest).parse(json)) {
    why = "its manifest is not valid JSON";
    return Read::Invalid;
  }
  why = validate(json, p);
  return why.empty() ? Read::Valid : Read::Invalid;
}

// ---------------------------------------------------------------- the record of what was placed

struct FileRecord {
  std::string path; // below the root ('/'); empty when the root is this file
  uint64_t size = 0, time = 0;
  std::string sha256;
};

struct RootRecord {
  std::string path;    // as the package names it (rtx-remix/mods/<mod>...)
  std::wstring target; // where it was placed
  std::string item, package, version, manifest;
  bool requiresRemix = false, createOnly = false;
  std::vector<FileRecord> files;
};

struct ScanRecord {
  uint64_t size = 0, time = 0;
  bool package = false;
};

bool itemId(const std::string &name) {
  return !name.empty() && name.size() <= 20 && name[0] != '0' &&
         std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// Item ids in numeric order (no leading zeros).
bool itemBefore(const std::string &a, const std::string &b) { return a.size() != b.size() ? a.size() < b.size() : a < b; }

bool number(const std::string &s) {
  return !s.empty() && s.size() <= 20 && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// The part of a root's path below its area: <mod>/... of
// rtx-remix/mods/<mod>/..., <add-on>/... of garrysmod/addons/<add-on>/....
std::string belowArea(const std::string &path) {
  const std::string folded = fold(path);
  for (const char *area : {kModsPrefix, kAddonsPrefix})
    if (startsWith(folded, area))
      return path.substr(std::strlen(area));
  return path;
}

bool addonRoot(const std::string &path) { return startsWith(fold(path), kAddonsPrefix); }

// shaderapi_proxy_workshop.txt: its header, then a record per line, fields
// separated by tabs:
//   scan <item> <size> <time> <0|1>    the item's archive when it was last read
//   root <path> <target> <item> <package> <version> <manifest SHA-256> <flags>
//   file <path below the root> <size> <time> <SHA-256>    of the root above
// A target inside the game root is kept relative to it, so an install moved
// or copied with its record still names its own folders. Flags:
// requires-remix, create-only (none in an older record).
struct State {
  std::wstring base; // the game root
  std::list<RootRecord> roots;
  std::map<std::string, ScanRecord> scans; // by item
  size_t dropped = 0;                      // lines load() left out

  // Failed: the record exists but cannot be used; nothing may be decided from
  // it, and it must not be written over.
  enum Load { Loaded, Failed };
  Load load(const std::wstring &path, std::string &why) {
    roots.clear();
    scans.clear();
    dropped = 0;
    if (!status(path).exists)
      return Loaded; // the first start
    std::string text;
    if (!readFile(path, text, uint64_t(256) << 20)) {
      why = "it could not be read";
      return Failed;
    }
    if (startsWith(text, "\xEF\xBB\xBF"))
      text.erase(0, 3); // a byte order mark an editor added
    bool header = text.empty();
    RootRecord *root = nullptr;
    for (size_t at = 0; at < text.size();) {
      size_t end = text.find('\n', at);
      if (end == std::string::npos)
        end = text.size();
      std::string line = text.substr(at, end - at);
      at = end + 1;
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      if (!header) {
        if (line != kStateHeader) {
          why = "it is not a record this proxy reads";
          return Failed;
        }
        header = true;
        continue;
      }
      if (line.empty())
        continue;
      const auto f = split(line, '\t');
      if (f[0] == "scan" && f.size() == 5 && itemId(f[1]) && number(f[2]) && number(f[3]) &&
          (f[4] == "0" || f[4] == "1")) {
        scans[f[1]] = {std::strtoull(f[2].c_str(), nullptr, 10), std::strtoull(f[3].c_str(), nullptr, 10), f[4] == "1"};
      } else if (f[0] == "root") {
        RootRecord r;
        root = nullptr;
        if ((f.size() == 7 || f.size() == 8) && rootRecord(f, r) && !find(r.target)) {
          roots.push_back(std::move(r));
          root = &roots.back();
        } else {
          ++dropped;
        }
      } else if (f[0] == "file" && root && f.size() == 5 && (f[1].empty() || !pathProblem(f[1])) && number(f[2]) &&
                 number(f[3]) && lowerHex64(f[4])) {
        root->files.push_back(
            {f[1], std::strtoull(f[2].c_str(), nullptr, 10), std::strtoull(f[3].c_str(), nullptr, 10), f[4]});
      } else {
        ++dropped;
      }
    }
    if (dropped)
      say("%s: %zu lines that are not valid records left out", narrow(path).c_str(), dropped);
    return Loaded;
  }

  bool save(const std::wstring &path) const {
    std::string text = std::string(kStateHeader) + "\n";
    char digits[64];
    for (const auto &[item, scan] : scans) {
      snprintf(digits, sizeof(digits), "\t%llu\t%llu\t%d\n", (unsigned long long)scan.size,
               (unsigned long long)scan.time, scan.package ? 1 : 0);
      text += "scan\t" + item + digits;
    }
    for (const RootRecord &root : roots) {
      const std::wstring &t = root.target;
      std::string flags = root.requiresRemix ? "requires-remix" : "";
      if (root.createOnly)
        flags += flags.empty() ? "create-only" : ",create-only";
      text += "root\t" + root.path + "\t" + narrow(t.size() > base.size() + 1 && overlaps(t, base) ? t.substr(base.size() + 1) : t) +
              "\t" + root.item + "\t" + root.package + "\t" + root.version + "\t" + root.manifest + "\t" + flags + "\n";
      for (const FileRecord &file : root.files) {
        snprintf(digits, sizeof(digits), "\t%llu\t%llu\t", (unsigned long long)file.size, (unsigned long long)file.time);
        text += "file\t" + file.path + digits + file.sha256 + "\n";
      }
    }
    // A whole new record or the old one.
    const std::wstring temp = path + L".tmp";
    HANDLE f = CreateFileW(longPath(temp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE)
      return false;
    DWORD wrote = 0;
    const bool ok = WriteFile(f, text.data(), DWORD(text.size()), &wrote, nullptr) && wrote == text.size() &&
                    FlushFileBuffers(f);
    CloseHandle(f);
    return ok && MoveFileExW(longPath(temp).c_str(), longPath(path).c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
  }

  RootRecord *find(const std::wstring &target) {
    for (RootRecord &root : roots)
      if (samePath(root.target, target))
        return &root;
    return nullptr;
  }

private:
  // A root line, checked before anything is done with it: a path a package
  // could name, an item id, and a target with no empty, . or .. component
  // that is that path's place (<area>\<mod>..., or the game's
  // garrysmod\addons\<add-on>...).
  bool rootRecord(const std::vector<std::string> &f, RootRecord &r) const {
    const std::string folded = fold(f[1]);
    if (pathProblem(f[1]) || !(startsWith(folded, kModsPrefix) || addonRoot(f[1])) ||
        std::count(folded.begin(), folded.end(), '/') < 2 || !itemId(f[3]))
      return false;
    std::string rest = f[2];
    const bool drive = rest.size() > 3 && std::isalpha(static_cast<unsigned char>(rest[0])) && rest[1] == ':' &&
                       rest[2] == '\\';
    const bool unc = !drive && startsWith(rest, "\\\\");
    rest.erase(0, drive ? 3 : unc ? 2 : 0);
    for (const std::string &part : split(rest, '\\'))
      if (part.empty() || part == "." || part == ".." || part.find('/') != std::string::npos)
        return false;
    r.target = drive || unc ? widen(f[2]) : base + L"\\" + widen(f[2]);
    const std::wstring tail = below(L"", belowArea(f[1]));
    const bool placed = addonRoot(f[1])
                            ? samePath(r.target, below(base, f[1]))
                            : r.target.size() > tail.size() && r.target[r.target.size() - tail.size() - 1] == L'\\' &&
                                  samePath(r.target.substr(r.target.size() - tail.size()), tail);
    if (!placed)
      return false;
    r.path = f[1];
    r.item = f[3];
    r.package = f[4];
    r.version = f[5];
    r.manifest = f[6];
    if (f.size() == 8)
      for (const std::string &flag : split(f[7], ',')) {
        r.requiresRemix = r.requiresRemix || flag == "requires-remix";
        r.createOnly = r.createOnly || flag == "create-only";
      }
    return true;
  }
};

// ---------------------------------------------------------------- Steam's records

// Whether an account on this computer subscribed to an item (1), not (0), or
// unknown (-1): Steam's appworkshop_4000.acf keeps details for every
// subscribed item, whose `subscribedby` lists the accounts. An item the game
// or a server downloaded (steamworks.DownloadUGC, which any server may call)
// is listed too, without a subscriber, and never counts (the native loader's
// ADR 0111).
int subscribed(const std::map<std::string, const Vdf *> *details, const std::string &item) {
  if (!details)
    return -1;
  const auto found = details->find(item);
  if (found == details->end())
    return 0;
  const Vdf *entry = found->second;
  if (!entry->block)
    return -1;
  const Vdf *by = entry->find("subscribedby");
  if (!by)
    return 0;
  if (by->block)
    return -1;
  bool account = false;
  for (std::string part : split(by->value, ',')) {
    while (!part.empty() && (part.front() == ' ' || part.front() == '\t'))
      part.erase(part.begin());
    while (!part.empty() && (part.back() == ' ' || part.back() == '\t'))
      part.pop_back();
    if (part.size() > 20 || !std::all_of(part.begin(), part.end(), [](char c) { return c >= '0' && c <= '9'; }))
      return -1;
    account = account || part.find_first_not_of('0') != std::string::npos;
  }
  return account ? 1 : 0;
}

std::wstring steamFromRegistry() {
  const HMODULE advapi = LoadLibraryExW(L"advapi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  using GetValue = LSTATUS(WINAPI *)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);
  const auto get = proc<GetValue>(advapi, "RegGetValueW");
  wchar_t path[1024] = {};
  DWORD size = sizeof(path);
  if (!get || get(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, path, &size) !=
                  ERROR_SUCCESS)
    return {};
  std::wstring steam = path;
  std::replace(steam.begin(), steam.end(), L'/', L'\\');
  return steam;
}

// The steamapps folder of the library holding GMod's Workshop items: the
// game's own when it is installed in one (<library>\steamapps\common\<game>)
// that has GMod's Workshop record, else (an install of its own, as
// RTXLauncher makes, or a copy in another library) the library Steam's
// libraryfolders.vdf lists app 4000 in, else any with GMod's Workshop folder.
std::wstring findLibrary(const Options &o, std::string &how) {
  if (!o.steamapps.empty()) {
    how = "as given";
    return o.steamapps;
  }
  const std::wstring parent = parentOf(o.gameRoot), grandparent = parentOf(parent);
  if (samePath(parent.substr(parent.find_last_of(L'\\') + 1), L"common") &&
      samePath(grandparent.substr(grandparent.find_last_of(L'\\') + 1), L"steamapps") &&
      status(grandparent + L"\\workshop\\appworkshop_4000.acf").exists) {
    how = "the game's library";
    return grandparent;
  }
  const std::wstring steam = o.steamRoot.empty() ? steamFromRegistry() : o.steamRoot;
  Vdf root;
  const Vdf *folders = !steam.empty() && loadVdf(steam + L"\\steamapps\\libraryfolders.vdf", root)
                           ? root.find("libraryfolders")
                           : nullptr;
  std::wstring fallback;
  for (size_t i = 0; folders && i < folders->children.size(); ++i) {
    const Vdf *path = folders->children[i].find("path"), *apps = folders->children[i].find("apps");
    if (!path || path->value.empty())
      continue;
    std::wstring steamapps = widen(path->value);
    std::replace(steamapps.begin(), steamapps.end(), L'/', L'\\');
    steamapps = (steamapps.back() == L'\\' ? steamapps : steamapps + L"\\") + L"steamapps";
    if (apps && apps->find("4000")) {
      how = "Steam's libraryfolders.vdf";
      return steamapps;
    }
    if (fallback.empty() && status(steamapps + L"\\workshop\\content\\4000").directory)
      fallback = steamapps;
  }
  how = "a library with GMod's Workshop folder";
  return fallback;
}

// ---------------------------------------------------------------- placing

struct Planned {
  const Package *package;
  const Root *root;
  std::wstring target;
};

// Writes an archive member to `temp`, beside where it goes, checking its size
// and SHA-256: only a whole file that is what the manifest says is ever put
// in place. Written through to the disk (about 15% slower than cached, a
// flush per file doubles the time), so a power cut cannot leave one whose
// size and time match the record but not its contents. On failure `temp` is
// gone.
bool extract(HANDLE archive, const Member &m, const std::wstring &temp, const std::string &sha256,
             std::vector<uint8_t> &buffer, std::string &why) {
  if (!makeDirectories(parentOf(temp))) {
    why = "its folder could not be made (error " + std::to_string(GetLastError()) + ")";
    return false;
  }
  HANDLE out = CreateFileW(longPath(temp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_WRITE_THROUGH, nullptr);
  if (out == INVALID_HANDLE_VALUE) {
    why = "error " + std::to_string(GetLastError()) + " making it";
    return false;
  }
  LARGE_INTEGER at;
  at.QuadPart = LONGLONG(m.offset);
  Sha256 sha;
  std::string failure;
  if (!sha.ok())
    failure = "SHA-256 is not available (bcrypt.dll)";
  else if (!SetFilePointerEx(archive, at, nullptr, FILE_BEGIN))
    failure = "error " + std::to_string(GetLastError()) + " reading the archive";
  for (uint64_t left = m.size; failure.empty() && left;) {
    const DWORD chunk = DWORD(std::min<uint64_t>(left, buffer.size()));
    DWORD got = 0, wrote = 0;
    if (!ReadFile(archive, buffer.data(), chunk, &got, nullptr)) {
      failure = "error " + std::to_string(GetLastError()) + " reading the archive";
    } else if (got != chunk) {
      failure = "the archive is shorter than its index (changed since it was read?)";
    } else if (sha.update(buffer.data(), got), !WriteFile(out, buffer.data(), got, &wrote, nullptr)) {
      failure = "error " + std::to_string(GetLastError()) + " writing it";
    } else if (wrote != got) {
      failure = "a short write";
    } else {
      left -= got;
      g_progress += got;
    }
  }
  CloseHandle(out);
  if (failure.empty() && sha.hex() != sha256)
    failure = "its SHA-256 differs from the manifest's";
  if (failure.empty())
    return true;
  why = failure;
  DeleteFileW(longPath(temp).c_str());
  return false;
}

// Whether the file at `path` is still what the record says was placed there.
bool unchanged(const std::wstring &path, const FileRecord &record, Status &s) {
  s = status(path);
  if (!s.exists || s.directory || s.size != record.size)
    return false;
  // Touched, perhaps not changed: its contents decide.
  return s.time == record.time || hashFile(path) == record.sha256;
}

// A file the runtime finds a mod by (mod.usda, .usdc or .usd, at any depth):
// put in place last, so a mod is never found half placed.
bool entryFile(const Root &root, const File &file) {
  const std::string path = fold(file.path.empty() ? root.path : file.path);
  const std::string name = path.substr(path.find_last_of('/') + 1);
  return name == "mod.usda" || name == "mod.usdc" || name == "mod.usd";
}

class Deployer {
public:
  explicit Deployer(Options options) : o_(std::move(options)) {
    state_.base = o_.gameRoot;
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stamp[32];
    swprintf(stamp, 32, L"%04u%02u%02u-%02u%02u%02u", unsigned(t.wYear), unsigned(t.wMonth), unsigned(t.wDay),
             unsigned(t.wHour), unsigned(t.wMinute), unsigned(t.wSecond));
    backup_ = parentOf(o_.statePath) + L"\\shaderapi_proxy_workshop_backup\\" + stamp;
  }
  ~Deployer() {
    if (lock_ != INVALID_HANDLE_VALUE)
      CloseHandle(lock_);
  }
  Deployer(const Deployer &) = delete;
  Deployer &operator=(const Deployer &) = delete;

  // The record alone. False when it cannot be used: then nothing is placed or
  // removed, and it is not written over.
  bool load() {
    std::string why;
    if (state_.load(o_.statePath, why) == State::Loaded)
      return true;
    say("%s: %s: nothing placed or removed (moved away, the proxy starts a new record, and leaves the folders it "
        "placed alone)",
        narrow(o_.statePath).c_str(), why.c_str());
    return false;
  }

  // The add-on folders (names below garrysmod/addons, upper case) the record
  // has for packages that require Remix.
  std::vector<std::wstring> rtxOnlyAddons() const {
    std::vector<std::wstring> names;
    for (const RootRecord &root : state_.roots)
      if (root.requiresRemix && addonRoot(root.path)) {
        const std::string rest = belowArea(root.path);
        const std::wstring name = upper(widen(rest.substr(0, rest.find('/'))));
        if (std::find(names.begin(), names.end(), name) == names.end())
          names.push_back(name);
      }
    return names;
  }

  // Steam's records, the packages and where each of their roots goes.
  void prepare() {
    // One game at a time places packages, until it is done with them (two
    // -multirun clients would each write the record from their own copy):
    // the other places nothing.
    lock_ = CreateFileW(longPath(o_.statePath + L".lock").c_str(), GENERIC_WRITE | DELETE, 0, nullptr, OPEN_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (lock_ == INVALID_HANDLE_VALUE) {
      const DWORD error = GetLastError();
      if (error == ERROR_SHARING_VIOLATION)
        say("another game is placing Workshop packages: nothing placed or removed");
      else
        say("%s.lock could not be made (error %lu): nothing placed or removed", narrow(o_.statePath).c_str(), error);
      load();
      return;
    }
    if (!load())
      return;
    std::string how;
    const std::wstring steamapps = findLibrary(o_, how);
    if (steamapps.empty()) {
      say("no Steam library with Garry's Mod's Workshop folder found: nothing placed or removed");
      return;
    }
    const std::wstring content = steamapps + L"\\workshop\\content\\4000";
    Vdf acf, nomount;
    const Vdf *app = loadVdf(steamapps + L"\\workshop\\appworkshop_4000.acf", acf) ? acf.find("AppWorkshop") : nullptr;
    const Vdf *detailsBlock = app ? app->find("WorkshopItemDetails") : nullptr;
    std::map<std::string, const Vdf *> details;
    if (detailsBlock && detailsBlock->block)
      for (const Vdf &entry : detailsBlock->children)
        details.emplace(entry.key, &entry);
    const auto *known = detailsBlock && detailsBlock->block ? &details : nullptr;
    std::set<std::string> disabled;
    if (loadVdf(o_.gameRoot + L"\\garrysmod\\cfg\\addonnomount.txt", nomount))
      if (const Vdf *entries = nomount.find("addonnomount"))
        for (const Vdf &entry : entries->children)
          if (!entry.block)
            disabled.insert(entry.value);

    // The items Steam has, and the ones something was placed from.
    std::set<std::string> items;
    for (const std::wstring &name : list(content, true))
      if (itemId(narrow(name)))
        items.insert(narrow(name));
    for (const RootRecord &root : state_.roots)
      items.insert(root.item);
    std::vector<std::string> order(items.begin(), items.end());
    std::sort(order.begin(), order.end(), itemBefore);

    std::map<std::string, ScanRecord> scans;
    size_t subscriptions = 0;
    for (const std::string &item : order) {
      const int sub = subscribed(known, item);
      subscriptions += sub == 1;
      if (sub == 0 || disabled.count(item))
        continue; // what was placed from it goes
      const auto cached = state_.scans.find(item);
      const bool placedFrom =
          std::any_of(state_.roots.begin(), state_.roots.end(), [&](const RootRecord &r) { return r.item == item; });
      const auto keep = [&](const char *why) {
        for (const RootRecord &root : state_.roots)
          if (root.item == item) {
            kept_.push_back(root.target);
            say("item %s: %s; %s stays as it is", item.c_str(), why, root.path.c_str());
          }
      };
      if (sub < 0) {
        if (cached != state_.scans.end())
          scans[item] = cached->second; // as known: Steam's record says nothing new
        keep("Steam's record does not say whether it is subscribed");
        continue;
      }
      const std::wstring folder = content + L"\\" + widen(item);
      const auto archives = list(folder, false, ".gma");
      if (archives.size() != 1) {
        keep(archives.empty() ? "not downloaded" : "more than one .gma in its folder");
        continue;
      }
      const std::wstring archive = folder + L"\\" + archives[0];
      const Status s = status(archive);
      if (cached != state_.scans.end() && !cached->second.package && cached->second.size == s.size &&
          cached->second.time == s.time) {
        scans[item] = cached->second;
        continue;
      }
      Package &p = packages_.emplace_back();
      std::string why;
      const Read read = readPackage(item, archive, p, why);
      // A plain or unreadable archive is not read again until it changes. One
      // that something was placed from is (Steam may be replacing it).
      if (read == Read::Valid || read == Read::NotPackage || (read == Read::Unreadable && !placedFrom))
        scans[item] = {s.size, s.time, read == Read::Valid};
      if (read == Read::Valid)
        continue;
      packages_.pop_back();
      if (read == Read::NotPackage)
        continue;
      if (read == Read::Unreadable && !placedFrom) {
        say("item %s (%s): skipped until it changes: %s", item.c_str(), narrow(archives[0]).c_str(), why.c_str());
        continue;
      }
      say("item %s (%s): not placed: %s", item.c_str(), narrow(archives[0]).c_str(), why.c_str());
      keep("its package could not be used");
    }
    const bool rescanned =
        scans.size() != state_.scans.size() ||
        !std::equal(scans.begin(), scans.end(), state_.scans.begin(), [](const auto &a, const auto &b) {
          return a.first == b.first && a.second.size == b.second.size && a.second.time == b.second.time &&
                 a.second.package == b.second.package;
        });
    state_.scans = std::move(scans);
    if (!known)
      say("%s: Steam keeps no Workshop item details, so no item counts as subscribed", narrow(steamapps).c_str());

    // Where each root goes. Of two packages with one id, or with roots that
    // overlap, the first item's is placed; a root the record keeps holds its
    // place. A package that is not placed takes no id.
    std::set<std::string> ids;
    for (const Package &p : packages_) {
      std::vector<Planned> roots;
      std::string clash;
      if (ids.count(p.id))
        clash = "an earlier item has the same package id";
      for (const Root &root : p.roots) {
        if (!root.addon && o_.modsDir.empty())
          continue; // the runtime reads no mods: the ones placed before stay as they are
        const std::wstring target =
            root.addon ? below(o_.gameRoot, root.path) : below(o_.modsDir, belowArea(root.path));
        for (const std::wstring &other : kept_)
          if (clash.empty() && overlaps(target, other))
            clash = "it overlaps " + narrow(other) + ", which stays as it is";
        for (const Planned &other : plan_)
          if (clash.empty() && overlaps(target, other.target))
            clash = "it overlaps " + other.root->path + " of item " + other.package->item;
        for (const Planned &other : roots)
          if (clash.empty() && overlaps(target, other.target))
            clash = "its " + other.root->path + " and " + root.path + " would go to one folder";
        roots.push_back({&p, &root, target});
      }
      if (!clash.empty()) {
        say("item %s (%s %s): not placed: %s", p.item.c_str(), p.id.c_str(), p.version.c_str(), clash.c_str());
        continue;
      }
      ids.insert(p.id);
      plan_.insert(plan_.end(), roots.begin(), roots.end());
      say("item %s: %s %s, %zu root file%s", p.item.c_str(), p.id.c_str(), p.version.c_str(), p.roots.size(),
          p.roots.size() == 1 ? "" : "s");
    }
    say("%s (%s): %zu subscribed items, %zu content packages", narrow(steamapps).c_str(), how.c_str(), subscriptions,
        packages_.size());
    ready_ = true;
    if (rescanned || state_.dropped)
      save();
  }

  // Add-on folders, before GMod mounts garrysmod/addons.
  void addons() { apply(true); }
  // Remix mods, before the runtime reads the mods directory. Without one the
  // mods placed before stay as they are.
  void mods() {
    if (!o_.modsDir.empty())
      apply(false);
  }

private:
  Options o_;
  State state_;
  std::list<Package> packages_;
  std::vector<Planned> plan_;
  std::vector<std::wstring> kept_;
  std::wstring backup_; // this start's backup folder
  HANDLE lock_ = INVALID_HANDLE_VALUE;
  bool ready_ = false;

  bool save() {
    if (state_.save(o_.statePath))
      return true;
    say("could not write %s (error %lu)", narrow(o_.statePath).c_str(), GetLastError());
    return false;
  }

  void apply(bool addons) {
    if (!ready_)
      return;
    // What no longer belongs here goes first, which also frees its space.
    for (auto it = state_.roots.begin(); it != state_.roots.end();) {
      const auto same = [&](const std::wstring &target) { return samePath(target, it->target); };
      const bool stays = std::any_of(plan_.begin(), plan_.end(), [&](const Planned &p) { return same(p.target); }) ||
                         std::any_of(kept_.begin(), kept_.end(), same);
      if (stays || addonRoot(it->path) != addons) {
        ++it;
        continue;
      }
      const size_t before = it->files.size();
      remove(*it);
      // Kept: files that could not be taken away (tried again at the next
      // start), or a folder with someone else's files in it, which stays the
      // proxy's (subscribed again, the package is placed around them).
      const bool gone = it->files.empty() && !status(it->target).exists;
      const bool changed = gone || it->files.size() != before;
      it = gone ? state_.roots.erase(it) : std::next(it);
      if (changed)
        save();
    }
    for (const Planned &p : plan_)
      if (p.root->addon == addons)
        place(p);
  }

  // Moves a file the proxy does not keep where it is (one it placed that
  // changed since, or someone else's in a package file's place) into this
  // start's backup folder: no change is lost.
  bool backup(const std::string &rootPath, const std::string &filePath, const std::wstring &path) {
    if (!status(path).exists)
      return true;
    const std::wstring to = below(below(backup_, rootPath), filePath);
    if (makeDirectories(parentOf(to)) &&
        MoveFileExW(longPath(path).c_str(), longPath(to).c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH))
      return true;
    const DWORD error = GetLastError();
    say("%s could not be moved to %s (error %lu): it stays", narrow(path).c_str(), narrow(to).c_str(), error);
    return false;
  }

  // The folders above a root that placing it made (rtx-remix/mods/<pack> of
  // rtx-remix/mods/<pack>/<mod>) once they are empty; never its area.
  static void removeEmptyParents(const RootRecord &root) {
    const std::string rest = belowArea(root.path);
    std::wstring dir = root.target;
    for (auto depth = std::count(rest.begin(), rest.end(), '/'); depth > 0; --depth) {
      dir = parentOf(dir);
      const DWORD attributes = GetFileAttributesW(longPath(dir).c_str());
      if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
          !RemoveDirectoryW(longPath(dir).c_str()))
        break;
    }
  }

  // Takes away what was placed for a root: its files as placed, a
  // replace-owned one changed since into the backup (a create-only one stays
  // the player's), and the folders that leaves empty. What could not be taken
  // away stays in `root.files`.
  void remove(RootRecord &root) {
    size_t removed = 0, backedUp = 0, theirs = 0;
    std::vector<FileRecord> left;
    for (const FileRecord &file : root.files) {
      const std::wstring path = below(root.target, file.path);
      Status s;
      if (unchanged(path, file, s)) {
        if (DeleteFileW(longPath(path).c_str()))
          ++removed;
        else
          left.push_back(file);
      } else if (s.exists && !s.directory) {
        if (root.createOnly)
          ++theirs;
        else if (backup(root.path, file.path, path))
          ++backedUp;
        else
          left.push_back(file);
      }
    }
    const bool had = !root.files.empty();
    root.files = std::move(left);
    const bool gone = removeEmptyFolders(root.target) || !status(root.target).exists;
    if (gone)
      removeEmptyParents(root);
    if (!had)
      return; // taken away before; what stays is someone else's
    std::string line = root.path + " (item " + root.item + ", " + root.package + "): " + std::to_string(removed) +
                       " files removed";
    if (backedUp)
      line += ", " + std::to_string(backedUp) + " changed since they were placed moved to " + narrow(backup_);
    if (theirs)
      line += ", " + std::to_string(theirs) + " create-only files changed since they were placed stay";
    if (!root.files.empty())
      line += ", " + std::to_string(root.files.size()) + " could not be removed (tried again at the next start)";
    else if (!gone)
      line += ", files someone else put there stay";
    say("%s", line.c_str());
  }

  void commit(RootRecord *record, const std::map<std::string, FileRecord> &placed, bool &dirty) {
    if (!record || !dirty)
      return;
    record->files.clear();
    for (const auto &entry : placed)
      record->files.push_back(entry.second);
    save();
    dirty = false;
  }

  void place(const Planned &p) {
    const Root &root = *p.root;
    const Package &package = *p.package;
    const std::string label = root.path + " (item " + package.item + ", " + package.id + " " + package.version + ")";
    RootRecord *record = state_.find(p.target);
    if (!record) {
      // A folder is the proxy's once its record says so. One with files in
      // it, or a file there, is someone else's.
      const Status s = status(p.target);
      if (s.exists && (!s.directory || holdsFiles(p.target))) {
        say("%s: left alone: %s was not placed by the proxy (with it gone, the proxy places the package)",
            label.c_str(), narrow(p.target).c_str());
        return;
      }
    }
    std::map<std::string, FileRecord> placed; // by folded path
    bool dirty = false;
    if (record) {
      for (const FileRecord &file : record->files)
        placed[fold(file.path)] = file;
      dirty = record->path != root.path || record->item != package.item || record->package != package.id ||
              record->requiresRemix != package.requiresRemix || record->createOnly != root.createOnly;
      record->path = root.path;
      record->item = package.item;
      record->package = package.id;
      record->requiresRemix = package.requiresRemix;
      record->createOnly = root.createOnly;
    }

    // What each file needs. One as the proxy placed it is replaced by a new
    // version; one that already is the package's is taken as placed; a
    // missing one is written. A changed one, or someone else's, stays while
    // it is a create-only file or an edit of a file the package has not
    // changed since; otherwise the package's goes in, and the changed one into
    // the backup.
    struct Job {
      const File *file;
      std::wstring path, temp;
      bool backup;
    };
    std::vector<Job> jobs;
    size_t current = 0, stayed = 0;
    uint64_t bytes = 0;
    std::string firstStayed;
    for (const File &file : root.files) {
      const std::wstring path = below(p.target, file.path);
      const std::string key = fold(file.path);
      const auto found = placed.find(key);
      const bool recorded = found != placed.end();
      Status s;
      if (recorded && unchanged(path, found->second, s)) {
        if (found->second.time != s.time) {
          found->second.time = s.time; // touched, not changed
          dirty = true;
        }
        if (found->second.sha256 == file.sha256 || root.createOnly) {
          ++current;
          continue;
        }
        jobs.push_back({&file, path, {}, false});
      } else {
        if (!recorded)
          s = status(path);
        if (!s.exists) {
          jobs.push_back({&file, path, {}, false});
        } else if (!s.directory && s.size == file.size && hashFile(path) == file.sha256) {
          placed[key] = {file.path, s.size, s.time, file.sha256}; // already the package's
          dirty = true;
          ++current;
          continue;
        } else if (s.directory || root.createOnly || (recorded && found->second.sha256 == file.sha256)) {
          if (recorded && s.directory) {
            placed.erase(found); // a folder now: not the proxy's file
            dirty = true;
          }
          if (!stayed++)
            firstStayed = file.path;
          continue;
        } else {
          jobs.push_back({&file, path, {}, true});
        }
      }
      bytes += file.size;
    }
    if (!record && jobs.empty()) {
      say("%s: nothing to place (folders where its files go)", label.c_str());
      return;
    }
    std::stable_partition(jobs.begin(), jobs.end(), [&](const Job &job) { return !entryFile(root, *job.file); });

    size_t written = 0, failed = 0, dropped = 0, backedUp = 0, undeletable = 0;
    std::string firstFailure;
    const auto fail = [&](const std::string &what) {
      if (!failed++)
        firstFailure = what;
    };
    const ULONGLONG started = GetTickCount64();
    if (!jobs.empty()) {
      // Room for the new files beside the old ones.
      std::wstring existing = p.target;
      while (!existing.empty() && !status(existing).exists)
        existing = parentOf(existing);
      ULARGE_INTEGER available = {};
      if (GetDiskFreeSpaceExW(longPath(existing + L"\\").c_str(), &available, nullptr, nullptr) &&
          available.QuadPart < bytes + kDiskMargin) {
        say("%s: not placed: it needs %s, and the drive has %s free", label.c_str(), gib(bytes).c_str(),
            gib(available.QuadPart).c_str());
        commit(record, placed, dirty);
        return;
      }
      HANDLE archive = CreateFileW(longPath(package.archivePath).c_str(), GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
      if (archive == INVALID_HANDLE_VALUE) {
        say("%s: not placed: its archive could not be opened (error %lu)", label.c_str(), GetLastError());
        commit(record, placed, dirty);
        return;
      }
      if (!record) {
        // The folder is the proxy's from here on, once the record says so.
        state_.roots.push_back(
            {root.path, p.target, package.item, package.id, {}, {}, package.requiresRemix, root.createOnly, {}});
        record = &state_.roots.back();
        if (!save()) {
          state_.roots.pop_back();
          CloseHandle(archive);
          say("%s: not placed: the record could not be written", label.c_str());
          return;
        }
      }
      // Every new file is written and checked beside its place first (what a
      // start cut short left there goes): a version goes in whole or not at
      // all. Then each is put in place, the entry files last.
      removeEmptyFolders(p.target);
      std::vector<uint8_t> buffer(size_t(4) << 20);
      for (size_t i = 0; i < jobs.size() && !failed; ++i) {
        std::string why;
        const std::wstring temp = parentOf(jobs[i].path) + L"\\" + temporaryName(i);
        if (extract(archive, package.archive.members[jobs[i].file->member], temp, jobs[i].file->sha256, buffer, why))
          jobs[i].temp = temp;
        else
          fail(jobs[i].file->path + ": " + why);
      }
      CloseHandle(archive);
      for (Job &job : jobs) {
        if (job.temp.empty())
          continue;
        if (!failed) {
          if (job.backup && !backup(root.path, job.file->path, job.path)) {
            fail(job.file->path + ": what is in its place could not be moved to the backup");
          } else if (!MoveFileExW(longPath(job.temp).c_str(), longPath(job.path).c_str(), MOVEFILE_REPLACE_EXISTING)) {
            fail(job.file->path + ": error " + std::to_string(GetLastError()) + " putting it in place");
          } else {
            const Status s = status(job.path);
            placed[fold(job.file->path)] = {job.file->path, s.size, s.time, job.file->sha256};
            dirty = true;
            ++written;
            backedUp += job.backup;
            continue;
          }
        }
        DeleteFileW(longPath(job.temp).c_str());
      }
    }

    // The files this version no longer has go once it is in place (a changed
    // replace-owned one into the backup). One that cannot stays recorded, to
    // be tried again at the next start.
    std::set<std::string> wanted;
    for (const File &file : root.files)
      wanted.insert(fold(file.path));
    for (auto it = placed.begin(); !failed && it != placed.end();) {
      if (wanted.count(it->first)) {
        ++it;
        continue;
      }
      const std::wstring path = below(p.target, it->second.path);
      Status s;
      const bool ours = unchanged(path, it->second, s);
      const bool changed = !ours && s.exists && !s.directory && !root.createOnly;
      if ((ours && !DeleteFileW(longPath(path).c_str())) || (changed && !backup(root.path, it->second.path, path))) {
        ++undeletable;
        ++it;
        continue;
      }
      dropped += ours;
      backedUp += changed;
      it = placed.erase(it);
      dirty = true;
    }
    if (!failed && (record->version != package.version || record->manifest != package.manifest)) {
      record->version = package.version;
      record->manifest = package.manifest;
      dirty = true;
    }
    commit(record, placed, dirty);
    // Nothing of it placed: no claim or empty folders left behind.
    if (placed.empty() && removeEmptyFolders(p.target)) {
      removeEmptyParents(*record);
      state_.roots.remove_if([&](const RootRecord &r) { return &r == record; });
      save();
    }

    char took[32];
    snprintf(took, sizeof(took), "%.1f", double(GetTickCount64() - started) / 1000.0);
    std::string line = label + ": ";
    if (written)
      line += std::to_string(written) + " files written (" + gib(bytes) + " in " + took + " s), ";
    line += std::to_string(current) + " up to date";
    if (dropped)
      line += ", " + std::to_string(dropped) + " it no longer has removed";
    if (backedUp)
      line += ", " + std::to_string(backedUp) + " changed ones moved to " + narrow(backup_);
    if (stayed)
      line += ", " + std::to_string(stayed) + " changed or not the proxy's stay (first: " + firstStayed + ")";
    if (undeletable)
      line += ", " + std::to_string(undeletable) + " it no longer has could not be removed (tried again)";
    if (failed)
      line += ", FAILED (" + firstFailure + "): tried again at the next start";
    say("%s", line.c_str());
  }
};

// ---------------------------------------------------------------- add-ons that require Remix, without a runtime

// A session without a runtime leaves the add-on folders of packages that
// require Remix out of the searches filesystem_stdio.dll makes of
// garrysmod\addons, which is how GMod finds the folders it mounts: they stay
// on disk and in the record, and switching RTX back on costs nothing (the
// GMod native loader's ADR 0013 does the same).
using FindFirstFileAFn = HANDLE(WINAPI *)(LPCSTR, LPWIN32_FIND_DATAA);
using FindFirstFileExWFn = HANDLE(WINAPI *)(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
using FindNextFileAFn = BOOL(WINAPI *)(HANDLE, LPWIN32_FIND_DATAA);
using FindNextFileWFn = BOOL(WINAPI *)(HANDLE, LPWIN32_FIND_DATAW);
using FindCloseFn = BOOL(WINAPI *)(HANDLE);
FindFirstFileAFn g_findFirstFileA = FindFirstFileA;
FindFirstFileExWFn g_findFirstFileExW = FindFirstFileExW;
FindNextFileAFn g_findNextFileA = FindNextFileA;
FindNextFileWFn g_findNextFileW = FindNextFileW;
FindCloseFn g_findClose = FindClose;

std::wstring g_addonsDir;            // <game root>\garrysmod\addons
std::vector<std::wstring> g_rtxOnly; // folder names in it, upper case; set before any search is hooked
std::atomic<bool> g_hiding{false}, g_leftOut{false};
SRWLOCK g_searchLock = SRWLOCK_INIT;
std::set<HANDLE> g_searches; // open searches of g_addonsDir (under g_searchLock)
std::atomic<size_t> g_searchCount{0};

std::wstring fromAnsi(const char *s) {
  const int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
  if (n <= 1)
    return {};
  std::wstring out(size_t(n), L'\0');
  MultiByteToWideChar(CP_ACP, 0, s, -1, out.data(), n);
  out.resize(size_t(n - 1));
  return out;
}

bool searchesAddons(std::wstring pattern) {
  if (!g_hiding)
    return false;
  std::replace(pattern.begin(), pattern.end(), L'/', L'\\');
  const size_t slash = pattern.find_last_of(L'\\');
  return samePath(fullPath(slash == std::wstring::npos ? std::wstring(L".") : pattern.substr(0, slash + 1)),
                  g_addonsDir);
}

bool leftOut(const wchar_t *name, DWORD attributes) {
  if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
      std::find(g_rtxOnly.begin(), g_rtxOnly.end(), upper(name)) == g_rtxOnly.end())
    return false;
  if (!g_leftOut.exchange(true))
    say("a search of garrysmod\\addons left %s out (the first one)", narrow(name).c_str());
  return true;
}

void track(HANDLE search, bool open) {
  AcquireSRWLockExclusive(&g_searchLock);
  if (open)
    g_searches.insert(search);
  else
    g_searches.erase(search);
  g_searchCount = g_searches.size();
  ReleaseSRWLockExclusive(&g_searchLock);
}

bool tracked(HANDLE search) {
  if (!g_searchCount)
    return false;
  AcquireSRWLockShared(&g_searchLock);
  const bool found = g_searches.count(search) != 0;
  ReleaseSRWLockShared(&g_searchLock);
  return found;
}

// Every entry of a search was left out.
HANDLE noneFound(HANDLE search) {
  g_findClose(search);
  SetLastError(ERROR_FILE_NOT_FOUND);
  return INVALID_HANDLE_VALUE;
}

HANDLE WINAPI findFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA data) {
  const HANDLE search = g_findFirstFileA(pattern, data);
  if (search == INVALID_HANDLE_VALUE || !pattern || !searchesAddons(fromAnsi(pattern)))
    return search;
  while (leftOut(fromAnsi(data->cFileName).c_str(), data->dwFileAttributes))
    if (!g_findNextFileA(search, data))
      return noneFound(search);
  track(search, true);
  return search;
}

HANDLE WINAPI findFirstFileExW(LPCWSTR pattern, FINDEX_INFO_LEVELS level, LPVOID data, FINDEX_SEARCH_OPS op,
                               LPVOID filter, DWORD flags) {
  const HANDLE search = g_findFirstFileExW(pattern, level, data, op, filter, flags);
  if (search == INVALID_HANDLE_VALUE || !pattern || !searchesAddons(pattern))
    return search;
  auto *found = static_cast<LPWIN32_FIND_DATAW>(data);
  while (leftOut(found->cFileName, found->dwFileAttributes))
    if (!g_findNextFileW(search, found))
      return noneFound(search);
  track(search, true);
  return search;
}

BOOL WINAPI findNextFileA(HANDLE search, LPWIN32_FIND_DATAA data) {
  for (;;) {
    if (!g_findNextFileA(search, data))
      return FALSE;
    if (!tracked(search) || !leftOut(fromAnsi(data->cFileName).c_str(), data->dwFileAttributes))
      return TRUE;
  }
}

BOOL WINAPI findNextFileW(HANDLE search, LPWIN32_FIND_DATAW data) {
  for (;;) {
    if (!g_findNextFileW(search, data))
      return FALSE;
    if (!tracked(search) || !leftOut(data->cFileName, data->dwFileAttributes))
      return TRUE;
  }
}

BOOL WINAPI findClose(HANDLE search) {
  if (tracked(search))
    track(search, false);
  return g_findClose(search);
}

template <typename Fn> void hookImport(HMODULE module, const char *name, Fn &original, Fn hook) {
  if (void *old = proxy::d3d::patchModuleImport(module, "KERNEL32.dll", name, reinterpret_cast<void *>(hook)))
    original = reinterpret_cast<Fn>(old);
}

void hideRtxOnly() {
  if (g_rtxOnly.empty() || g_hiding.exchange(true))
    return;
  std::string names;
  for (const std::wstring &name : g_rtxOnly)
    names += (names.empty() ? "" : ", ") + narrow(name);
  const HMODULE fs = GetModuleHandleW(L"filesystem_stdio.dll");
  if (!fs) {
    say("filesystem_stdio.dll is not loaded: the add-ons of packages that require Remix stay visible (%s)",
        names.c_str());
    return;
  }
  hookImport(fs, "FindFirstFileA", g_findFirstFileA, &findFirstFileA);
  hookImport(fs, "FindFirstFileExW", g_findFirstFileExW, &findFirstFileExW);
  hookImport(fs, "FindNextFileA", g_findNextFileA, &findNextFileA);
  hookImport(fs, "FindNextFileW", g_findNextFileW, &findNextFileW);
  hookImport(fs, "FindClose", g_findClose, &findClose);
  say("no runtime: GMod's searches of garrysmod\\addons leave out the add-ons of packages that require Remix (%s)",
      names.c_str());
}

// ---------------------------------------------------------------- the session

HANDLE g_thread = nullptr;
// No progress for this long and the runtime starts without the mods.
constexpr ULONGLONG kStall = 2 * 60 * 1000;

DWORD WINAPI modsThread(void *deployer) {
  std::unique_ptr<Deployer> d(static_cast<Deployer *>(deployer));
  try {
    d->mods();
  } catch (const std::exception &e) {
    say("unpacking the mods failed: %s", e.what());
  } catch (...) {
    say("unpacking the mods failed");
  }
  return 0;
}

} // namespace

void begin(bool runtime, const std::wstring &modsDir) {
  static bool begun = false;
  if (begun)
    return;
  begun = true;
  try {
    wchar_t value[MAX_PATH * 2] = {};
    if (GetEnvironmentVariableW(L"OPENREMIX_PROXY_WORKSHOP", value, MAX_PATH * 2) && value[0] == L'0') {
      say("off (OPENREMIX_PROXY_WORKSHOP=0)");
      return;
    }
    Options o;
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    // The game: garrysmod/ beside its executable (gmod_win64.exe). A tool in
    // bin\win64 that loads the proxy (hlmv, hammer) is not.
    o.gameRoot = parentOf(path);
    if (!status(o.gameRoot + L"\\garrysmod").directory) {
      say("%s is not the game (no garrysmod folder beside it): nothing placed", narrow(path).c_str());
      return;
    }
    o.modsDir = modsDir.empty() ? std::wstring() : fullPath(modsDir);
    // The record beside the proxy, as its other files.
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&begin), &self);
    GetModuleFileNameW(self, path, MAX_PATH);
    o.statePath = parentOf(path) + L"\\shaderapi_proxy_workshop.txt";
    g_addonsDir = o.gameRoot + L"\\garrysmod\\addons";
    auto d = std::make_unique<Deployer>(o);
    if (!runtime) {
      // The game as it is: nothing placed or removed, and what needs a
      // runtime out of sight.
      say("no runtime this session: nothing placed or removed");
      if (d->load())
        g_rtxOnly = d->rtxOnlyAddons();
      hideRtxOnly();
      return;
    }
    d->prepare();
    d->addons();
    g_rtxOnly = d->rtxOnlyAddons(); // should the runtime fail to start
    if (o.modsDir.empty()) {
      say("the runtime reads no mods (OPENREMIX_PROXY_MODS=0, or a directory the ANSI code page cannot name): the "
          "mods placed before stay as they are");
      return;
    }
    g_thread = CreateThread(nullptr, 0, modsThread, d.get(), 0, nullptr);
    if (g_thread)
      d.release(); // the thread's now
    else
      modsThread(d.release());
  } catch (const std::exception &e) {
    say("failed: %s", e.what());
  } catch (...) {
    say("failed");
  }
}

void finish() {
  if (!g_thread)
    return;
  const ULONGLONG started = GetTickCount64();
  if (WaitForSingleObject(g_thread, 500) == WAIT_TIMEOUT) {
    // The game's window thread waits (a first unpack of a remaster takes a
    // while): no "Not Responding" ghost offering to end the game meanwhile.
    DisableProcessWindowsGhosting();
    say("waiting for the mods to be unpacked");
    uint64_t seen = g_progress;
    ULONGLONG moved = GetTickCount64(), told = moved;
    while (WaitForSingleObject(g_thread, 1000) == WAIT_TIMEOUT) {
      const ULONGLONG now = GetTickCount64();
      if (const uint64_t progress = g_progress; progress != seen) {
        seen = progress;
        moved = now;
      } else if (now - moved >= kStall) {
        say("no progress for %llu s: the runtime starts without waiting", (unsigned long long)(kStall / 1000));
        CloseHandle(g_thread);
        g_thread = nullptr;
        return;
      }
      if (now - told >= 10000) {
        told = now;
        say("unpacking, %s read or written so far", gib(seen).c_str());
      }
    }
    say("waited %.1f s", double(GetTickCount64() - started) / 1000.0);
  }
  CloseHandle(g_thread);
  g_thread = nullptr;
}

void runtimeFailed() {
  try {
    hideRtxOnly();
  } catch (...) {
  }
}

void deploy(const Options &options) {
  Options o = options;
  o.gameRoot = fullPath(o.gameRoot);
  if (!o.modsDir.empty())
    o.modsDir = fullPath(o.modsDir);
  Deployer d(o);
  d.prepare();
  d.addons();
  d.mods();
}

std::vector<std::string> rtxOnlyAddons(const Options &options) {
  Options o = options;
  o.gameRoot = fullPath(o.gameRoot);
  Deployer d(o);
  std::vector<std::string> names;
  if (d.load())
    for (const std::wstring &name : d.rtxOnlyAddons())
      names.push_back(narrow(name));
  return names;
}

} // namespace proxy::workshop
