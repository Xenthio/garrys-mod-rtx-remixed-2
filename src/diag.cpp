#include "diag.h"

#include <dbghelp.h>
#include <psapi.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace proxy::diag {
namespace {

HANDLE g_file = INVALID_HANDLE_VALUE;
wchar_t g_dir[MAX_PATH] = {};
DWORD g_mainThread = 0;
ULONGLONG g_start = 0;
std::atomic<int> g_reports{0};
std::atomic<bool> g_reporting{false}, g_dumped{false};
LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;

using MiniDumpWriteDumpFn = BOOL(WINAPI *)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                           PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
MiniDumpWriteDumpFn g_writeDump = nullptr;

// Win32 writes, a line at a time: what was written survives the process.
void write(const char *text, size_t size) {
  if (g_file == INVALID_HANDLE_VALUE)
    return;
  DWORD written = 0;
  WriteFile(g_file, text, DWORD(size), &written, nullptr);
}

void line(const char *fmt, ...) {
  char text[1024];
  va_list args;
  va_start(args, fmt);
  int size = vsnprintf(text, sizeof(text) - 2, fmt, args);
  va_end(args);
  if (size < 0)
    return;
  if (size > int(sizeof(text)) - 3)
    size = int(sizeof(text)) - 3;
  text[size++] = '\r';
  text[size++] = '\n';
  write(text, size_t(size));
}

// "module.dll+0x1234" for an address inside a loaded module.
void describe(uintptr_t address, char *out, size_t size) {
  HMODULE module = nullptr;
  if (address && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCWSTR>(address), &module) &&
      module) {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(module, path, sizeof(path));
    const char *name = strrchr(path, '\\');
    snprintf(out, size, "%s+0x%llx", name ? name + 1 : path,
             static_cast<unsigned long long>(address - reinterpret_cast<uintptr_t>(module)));
    return;
  }
  snprintf(out, size, "0x%016llx (no module)", static_cast<unsigned long long>(address));
}

const char *codeName(DWORD code) {
  switch (code) {
  case EXCEPTION_ACCESS_VIOLATION: return "access violation";
  case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
  case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
  case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
  case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
  case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
  case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
  case 0xC0000374: return "heap corruption";
  case 0xC0000409: return "stack buffer overrun / fast fail";
  case 0xE06D7363: return "C++ exception";
  default: return "exception";
  }
}

// The codes a working game does not raise. C++ exceptions are thrown and
// caught in normal running, so they are reported only when nothing handles them.
bool fatal(DWORD code) {
  switch (code) {
  case EXCEPTION_ACCESS_VIOLATION:
  case EXCEPTION_ILLEGAL_INSTRUCTION:
  case EXCEPTION_PRIV_INSTRUCTION:
  case EXCEPTION_STACK_OVERFLOW:
  case EXCEPTION_INT_DIVIDE_BY_ZERO:
  case EXCEPTION_IN_PAGE_ERROR:
  case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
  case 0xC0000374:
  case 0xC0000409:
    return true;
  default:
    return false;
  }
}

void stackTrace(const CONTEXT &at) {
  CONTEXT context = at;
  line("  stack:");
  int outside = 0;
  for (int frame = 0; frame < 48 && context.Rip; ++frame) {
    char where[MAX_PATH + 32];
    describe(context.Rip, where, sizeof(where));
    line("    #%02d %s", frame, where);
    DWORD64 base = 0;
    if (PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &base, nullptr)) {
      void *handlerData = nullptr;
      DWORD64 establisher = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, context.Rip, function, &context, &handlerData, &establisher, nullptr);
    } else {
      // No unwind data: a leaf function, or generated code (then the frames
      // below are guesses, and the walk stops after a few of them).
      if (++outside > 3 || IsBadReadPtr(reinterpret_cast<void *>(context.Rsp), 8))
        break;
      context.Rip = *reinterpret_cast<DWORD64 *>(context.Rsp);
      context.Rsp += 8;
    }
  }
}

void modules() {
  HMODULE list[512];
  DWORD needed = 0;
  if (!K32EnumProcessModules(GetCurrentProcess(), list, sizeof(list), &needed))
    return;
  const DWORD count = needed / sizeof(HMODULE) < 512 ? needed / sizeof(HMODULE) : 512;
  line("  modules (%lu): base, size, link time, path", count);
  for (DWORD i = 0; i < count; ++i) {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(list[i], path, sizeof(path));
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(list[i]);
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(reinterpret_cast<const BYTE *>(list[i]) + dos->e_lfanew);
    line("    %016llx %8lx %08lx %s", reinterpret_cast<unsigned long long>(list[i]), nt->OptionalHeader.SizeOfImage,
         nt->FileHeader.TimeDateStamp, path);
  }
}

void dump(EXCEPTION_POINTERS *info) {
  if (!g_writeDump || g_dumped.exchange(true))
    return;
  wchar_t path[MAX_PATH];
  _snwprintf(path, MAX_PATH, L"%ls\\shaderapi_proxy_crash.dmp", g_dir);
  const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return;
  MINIDUMP_EXCEPTION_INFORMATION exception = {GetCurrentThreadId(), info, FALSE};
  const BOOL ok = g_writeDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                              MINIDUMP_TYPE(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules),
                              &exception, nullptr, nullptr);
  CloseHandle(file);
  line("  minidump: shaderapi_proxy_crash.dmp %s", ok ? "written" : "FAILED");
}

void report(EXCEPTION_POINTERS *info, const char *kind) {
  const EXCEPTION_RECORD &record = *info->ExceptionRecord;
  const CONTEXT &c = *info->ContextRecord;
  char where[MAX_PATH + 32];
  describe(reinterpret_cast<uintptr_t>(record.ExceptionAddress), where, sizeof(where));
  line("");
  line("==== %s: %s (0x%08lx) at %s", kind, codeName(record.ExceptionCode), record.ExceptionCode, where);
  line("  %.3f s after start, thread %lu%s", double(GetTickCount64() - g_start) / 1000.0, GetCurrentThreadId(),
       GetCurrentThreadId() == g_mainThread ? " (the thread that loaded the proxy)" : "");
  if ((record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION || record.ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
      record.NumberParameters >= 2)
    line("  %s address 0x%016llx",
         record.ExceptionInformation[0] == 0 ? "reading" : record.ExceptionInformation[0] == 1 ? "writing" : "executing",
         static_cast<unsigned long long>(record.ExceptionInformation[1]));
  line("  rax %016llx rbx %016llx rcx %016llx rdx %016llx", c.Rax, c.Rbx, c.Rcx, c.Rdx);
  line("  rsi %016llx rdi %016llx rbp %016llx rsp %016llx", c.Rsi, c.Rdi, c.Rbp, c.Rsp);
  line("  r8  %016llx r9  %016llx r10 %016llx r11 %016llx", c.R8, c.R9, c.R10, c.R11);
  line("  r12 %016llx r13 %016llx r14 %016llx r15 %016llx", c.R12, c.R13, c.R14, c.R15);
  stackTrace(c);
  // Once: the list is long and the same for later reports.
  static std::atomic<bool> listed{false};
  if (!listed.exchange(true))
    modules();
  dump(info);
}

// First chance: before any handler. A fault the game handles itself is
// reported too (the report says the game may have gone on), so only the first
// few are written.
LONG CALLBACK vectored(EXCEPTION_POINTERS *info) {
  if (!fatal(info->ExceptionRecord->ExceptionCode))
    return EXCEPTION_CONTINUE_SEARCH;
  if (g_reporting.exchange(true))
    return EXCEPTION_CONTINUE_SEARCH; // a fault while reporting
  if (g_reports.fetch_add(1) < 6)
    report(info, "FATAL EXCEPTION (first chance; if lines follow this report, the game handled it and went on)");
  g_reporting = false;
  return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI unhandled(EXCEPTION_POINTERS *info) {
  if (!g_reporting.exchange(true)) {
    if (fatal(info->ExceptionRecord->ExceptionCode) && g_reports.load() > 0) {
      line("==== UNHANDLED: the exception above (0x%08lx) ends the process", info->ExceptionRecord->ExceptionCode);
      dump(info);
    } else {
      report(info, "UNHANDLED EXCEPTION (ends the process)");
    }
    g_reporting = false;
  }
  return g_previousFilter && g_previousFilter != &unhandled ? g_previousFilter(info) : EXCEPTION_CONTINUE_SEARCH;
}

void header(HMODULE self) {
  line("shaderapidx9 proxy diagnostics | proxy built " __DATE__ " " __TIME__);
  SYSTEMTIME now;
  GetLocalTime(&now);
  line("started %04u-%02u-%02u %02u:%02u:%02u local", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
       now.wSecond);
  // RtlGetVersion: GetVersionEx reports the manifest's version, not the system's.
  if (const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
    using RtlGetVersionFn = LONG(WINAPI *)(OSVERSIONINFOW *);
    if (const auto version = reinterpret_cast<RtlGetVersionFn>(
            reinterpret_cast<void *>(GetProcAddress(ntdll, "RtlGetVersion")))) {
      OSVERSIONINFOW os = {};
      os.dwOSVersionInfoSize = sizeof(os);
      if (version(&os) == 0)
        line("Windows %lu.%lu build %lu%s", os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber,
             GetProcAddress(ntdll, "wine_get_version") ? " (Wine)" : "");
    }
  }
  SYSTEM_INFO system = {};
  GetSystemInfo(&system);
  MEMORYSTATUSEX memory = {};
  memory.dwLength = sizeof(memory);
  GlobalMemoryStatusEx(&memory);
  line("%lu logical processors | memory %llu MB, %llu MB available", system.dwNumberOfProcessors,
       memory.ullTotalPhys >> 20, memory.ullAvailPhys >> 20);
  DISPLAY_DEVICEA display = {};
  display.cb = sizeof(display);
  for (DWORD i = 0; EnumDisplayDevicesA(nullptr, i, &display, 0); ++i)
    if (display.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)
      line("display %lu: %s%s", i, display.DeviceString,
           display.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE ? " (primary)" : "");
  char path[MAX_PATH] = {};
  GetModuleFileNameA(self, path, sizeof(path));
  line("proxy: %s", path);
  GetModuleFileNameA(nullptr, path, sizeof(path));
  line("game: %s", path);
  line("command line: %s", GetCommandLineA());
  line("");
}

} // namespace

void install(HMODULE self) {
  if (g_file != INVALID_HANDLE_VALUE)
    return;
  g_start = GetTickCount64();
  g_mainThread = GetCurrentThreadId();
  GetModuleFileNameW(self, g_dir, MAX_PATH);
  if (wchar_t *slash = wcsrchr(g_dir, L'\\'))
    *slash = 0;
  wchar_t path[MAX_PATH];
  _snwprintf(path, MAX_PATH, L"%ls\\shaderapi_proxy_diag.log", g_dir);
  g_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  _snwprintf(path, MAX_PATH, L"%ls\\shaderapi_proxy_crash.dmp", g_dir);
  DeleteFileW(path); // a previous run's
  header(self);
  AddVectoredExceptionHandler(1, &vectored);
  g_previousFilter = SetUnhandledExceptionFilter(&unhandled);
}

void reinstall() {
  if (g_file == INVALID_HANDLE_VALUE)
    return;
  // Loaded here rather than in DllMain (the loader lock) or in the handler.
  if (!g_writeDump)
    if (const HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll"))
      g_writeDump = reinterpret_cast<MiniDumpWriteDumpFn>(
          reinterpret_cast<void *>(GetProcAddress(dbghelp, "MiniDumpWriteDump")));
  const auto previous = SetUnhandledExceptionFilter(&unhandled);
  if (previous != &unhandled)
    g_previousFilter = previous;
}

void stage(const char *fmt, ...) {
  if (g_file == INVALID_HANDLE_VALUE)
    return;
  char text[900];
  va_list args;
  va_start(args, fmt);
  vsnprintf(text, sizeof(text), fmt, args);
  va_end(args);
  line("[%9.3f] thread %5lu: %s", double(GetTickCount64() - g_start) / 1000.0, GetCurrentThreadId(), text);
}

} // namespace proxy::diag
