#include "profiler.h"

#include <windows.h>
#include <winternl.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace bridge {
namespace {

constexpr int kFrames = 24;
constexpr DWORD kSeconds = 5;
constexpr size_t kStackCopy = 64 * 1024;

std::string frameName(DWORD64 address) {
  HMODULE module = nullptr;
  char name[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(address), &module))
    GetModuleFileNameA(module, name, MAX_PATH);
  const char *base = strrchr(name, '\\');
  char out[MAX_PATH + 32];
  snprintf(out, sizeof out, "%s+0x%llx", base ? base + 1 : name,
           static_cast<unsigned long long>(address - reinterpret_cast<DWORD64>(module)));
  return out;
}

// Stack bounds from the thread's TEB, read while it runs.
bool stackBounds(HANDLE thread, DWORD64 &low, DWORD64 &high) {
  using QueryFn = NTSTATUS(NTAPI *)(HANDLE, THREADINFOCLASS, PVOID, ULONG, PULONG);
  static auto query = reinterpret_cast<QueryFn>(reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread")));
  struct {
    NTSTATUS exitStatus;
    PVOID teb;
    CLIENT_ID clientId;
    ULONG_PTR affinity;
    LONG priority, basePriority;
  } basic = {};
  if (!query || query(thread, static_cast<THREADINFOCLASS>(0), &basic, sizeof(basic), nullptr) != 0 || !basic.teb)
    return false;
  const auto *tib = static_cast<const NT_TIB *>(basic.teb);
  low = reinterpret_cast<DWORD64>(tib->StackLimit);
  high = reinterpret_cast<DWORD64>(tib->StackBase);
  return high > low;
}

// Unwinds a copied stack: registers that point into the original stack are
// rebased onto the copy, so nothing reads the (now running) thread's stack.
int unwind(CONTEXT context, const std::vector<uint8_t> &copy, DWORD64 origin, DWORD64 *frames) {
  const DWORD64 delta = reinterpret_cast<DWORD64>(copy.data()) - origin;
  auto inCopy = [&](DWORD64 a) { return a >= origin && a < origin + copy.size(); };
  context.Rsp += delta;
  if (inCopy(context.Rbp))
    context.Rbp += delta;
  int count = 0;
  while (count < kFrames && context.Rip) {
    frames[count++] = context.Rip;
    const DWORD64 rsp = context.Rsp - delta;
    if (!inCopy(rsp) || rsp + 8 > origin + copy.size())
      break;
    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
    if (!function) {
      context.Rip = *reinterpret_cast<const DWORD64 *>(context.Rsp);
      context.Rsp += 8;
      continue;
    }
    void *handlerData = nullptr;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData, &establisher,
                     nullptr);
    if (!inCopy(context.Rsp - delta))
      break;
  }
  return count;
}

void profile(const std::wstring &output) {
  std::map<std::vector<DWORD64>, unsigned> stacks;
  std::vector<uint8_t> copy(kStackCopy);
  const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
  const ULONGLONG end = GetTickCount64() + kSeconds * 1000;
  while (GetTickCount64() < end) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry = {};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
      if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self)
        continue;
      HANDLE thread =
          OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
      if (!thread)
        continue;
      DWORD64 low = 0, high = 0;
      if (stackBounds(thread, low, high) && SuspendThread(thread) != DWORD(-1)) {
        // Suspended: only the context and a plain copy of its stack. Anything
        // taking a lock (loader, heap) could wait on this very thread.
        CONTEXT context = {};
        context.ContextFlags = CONTEXT_FULL;
        size_t bytes = 0;
        if (GetThreadContext(thread, &context) && context.Rsp >= low && context.Rsp < high) {
          bytes = size_t(std::min<DWORD64>(high - context.Rsp, kStackCopy));
          std::memcpy(copy.data(), reinterpret_cast<const void *>(context.Rsp), bytes);
        }
        ResumeThread(thread);
        if (bytes) {
          std::vector<uint8_t> used(copy.begin(), copy.begin() + bytes);
          DWORD64 frames[kFrames];
          const int count = unwind(context, used, context.Rsp, frames);
          if (count)
            ++stacks[std::vector<DWORD64>(frames, frames + count)];
        }
      }
      CloseHandle(thread);
    }
    CloseHandle(snapshot);
    Sleep(1);
  }
  FILE *f = _wfopen(output.c_str(), L"wb");
  if (!f)
    return;
  for (const auto &[frames, count] : stacks) {
    fprintf(f, "%u", count);
    for (size_t i = 0; i < frames.size(); ++i)
      fprintf(f, "%c%s", i ? ';' : ' ', frameName(frames[i]).c_str());
    fprintf(f, "\n");
  }
  fclose(f);
}

} // namespace

void startProfilerWatch(const std::wstring &directory) {
  std::thread([directory] {
    const std::wstring request = directory + L"\\openremix_profile.request";
    for (;;) {
      Sleep(250);
      if (GetFileAttributesW(request.c_str()) == INVALID_FILE_ATTRIBUTES)
        continue;
      DeleteFileW(request.c_str());
      profile(directory + L"\\openremix_profile.txt");
    }
  }).detach();
}

} // namespace bridge
