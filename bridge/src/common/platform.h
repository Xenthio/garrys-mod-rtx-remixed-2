#pragma once
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <string>

namespace bridge {

// Object names derive from the client pid and a per-session nonce.
inline std::wstring objectName(DWORD clientPid, DWORD nonce, const wchar_t *suffix) {
  wchar_t name[128];
  swprintf(name, 128, L"Local\\OpenRemixBridge.%lu.%lu.%ls", static_cast<unsigned long>(clientPid),
           static_cast<unsigned long>(nonce), suffix);
  return name;
}

// Minimal line logger: file next to the module plus OutputDebugString.
class Log {
public:
  void open(const std::wstring &path) {
    if (!m_file)
      m_file = _wfopen(path.c_str(), L"w");
  }
  void write(const char *fmt, ...) {
    char line[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    OutputDebugStringA(line);
    if (m_file) {
      fprintf(m_file, "%s\n", line);
      fflush(m_file);
    }
  }
  ~Log() {
    if (m_file)
      fclose(m_file);
  }

private:
  FILE *m_file = nullptr;
};

inline std::wstring moduleDirectory(HMODULE module) {
  wchar_t path[MAX_PATH * 2];
  DWORD n = GetModuleFileNameW(module, path, MAX_PATH * 2);
  std::wstring s(path, n);
  auto slash = s.find_last_of(L"\\/");
  return slash == std::wstring::npos ? L"." : s.substr(0, slash);
}

} // namespace bridge
