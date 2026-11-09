#include "client/transport.h"

#include <cstring>
#include <new>

namespace bridge::client {
using namespace bridge::proto;

namespace {
std::wstring hostPath() {
  wchar_t env[MAX_PATH * 2];
  DWORD n = GetEnvironmentVariableW(L"OPENREMIX_BRIDGE_HOST", env, MAX_PATH * 2);
  if (n > 0 && n < MAX_PATH * 2)
    return std::wstring(env, n);
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&hostPath), &self);
  return moduleDirectory(self) + L"\\openremix\\openremix_bridge_host.exe";
}

uint32_t ringSizeFromEnv() {
  char value[32];
  DWORD n = GetEnvironmentVariableA("OPENREMIX_BRIDGE_RING_MB", value, sizeof(value));
  if (n > 0 && n < sizeof(value)) {
    unsigned long mb = strtoul(value, nullptr, 10);
    if (mb >= 8 && mb <= 512)
      return static_cast<uint32_t>(mb) * 1024u * 1024u;
  }
  return kDefaultRingSize;
}
} // namespace

int32_t Transport::start(Log &log, std::string &error) {
  m_log = &log;
  m_ringSize = ringSizeFromEnv();
  const DWORD pid = GetCurrentProcessId();
  const DWORD nonce = GetTickCount() ^ (pid << 16) ^ static_cast<DWORD>(reinterpret_cast<uintptr_t>(this));
  const uint64_t total = uint64_t(kHeaderSize) + kReplySize + m_ringSize;

  m_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(total),
                                 objectName(pid, nonce, L"shm").c_str());
  if (!m_mapping) {
    error = "CreateFileMapping failed: " + std::to_string(GetLastError());
    return 1;
  }
  auto *base = static_cast<uint8_t *>(MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
  if (!base) {
    error = "MapViewOfFile failed: " + std::to_string(GetLastError());
    stop();
    return 1;
  }
  m_header = new (base) Header();
  m_reply = base + kHeaderSize;
  m_ring = m_reply + kReplySize;
  m_header->magic = kMagic;
  m_header->version = kVersion;
  m_header->headerSize = kHeaderSize;
  m_header->replySize = kReplySize;
  m_header->ringSize = m_ringSize;
  m_header->clientPid = pid;

  m_workEvent = CreateEventW(nullptr, FALSE, FALSE, objectName(pid, nonce, L"work").c_str());
  m_doneEvent = CreateEventW(nullptr, FALSE, FALSE, objectName(pid, nonce, L"done").c_str());
  if (!m_workEvent || !m_doneEvent) {
    error = "CreateEvent failed: " + std::to_string(GetLastError());
    stop();
    return 1;
  }

  const std::wstring exe = hostPath();
  const std::wstring dir = exe.substr(0, exe.find_last_of(L"\\/"));
  std::wstring cmd = L"\"" + exe + L"\" " + std::to_wstring(pid) + L" " + std::to_wstring(nonce);
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};
  if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                      nullptr, dir.c_str(), &si, &pi)) {
    error = "failed to launch bridge host (" + std::to_string(GetLastError()) + ")";
    stop();
    return 2; // REMIXAPI_ERROR_CODE_LOAD_LIBRARY_FAILURE
  }
  // Kill the host together with the game, even if the game crashes.
  m_job = CreateJobObjectW(nullptr, nullptr);
  if (m_job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(m_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    if (!AssignProcessToJobObject(m_job, pi.hProcess))
      log.write("bridge: host not assigned to job (%lu)", GetLastError());
  }
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);
  m_hostProcess = pi.hProcess;
  log.write("bridge: launched host pid %lu, ring %u MiB", pi.dwProcessId, m_ringSize >> 20);

  const DWORD deadline = GetTickCount() + 60000;
  while (m_header->hostState.load() == static_cast<uint32_t>(HostState::Starting)) {
    if (WaitForSingleObject(m_hostProcess, 10) == WAIT_OBJECT_0) {
      DWORD code = 0;
      GetExitCodeProcess(m_hostProcess, &code);
      error = "bridge host exited during startup (" + std::to_string(code) + ")";
      m_hostDead = true;
      return 1;
    }
    if (static_cast<int32_t>(GetTickCount() - deadline) > 0) {
      error = "bridge host did not become ready";
      TerminateProcess(m_hostProcess, 1);
      m_hostDead = true;
      return 1;
    }
  }
  if (m_header->hostState.load() != static_cast<uint32_t>(HostState::Ready)) {
    error = std::string("bridge host failed: ") + m_header->lastError;
    m_hostDead = true;
    return 2;
  }
  return 0;
}

void Transport::stop() {
  if (m_header && m_hostProcess && !m_hostDead) {
    m_header->clientClosing.store(1);
    SetEvent(m_workEvent);
    if (WaitForSingleObject(m_hostProcess, 5000) != WAIT_OBJECT_0)
      TerminateProcess(m_hostProcess, 1);
  }
  if (m_header)
    UnmapViewOfFile(m_header);
  for (HANDLE *h : {&m_mapping, &m_workEvent, &m_doneEvent, &m_hostProcess, &m_job}) {
    if (*h)
      CloseHandle(*h);
    *h = nullptr;
  }
  m_header = nullptr;
  m_reply = m_ring = nullptr;
}

template <class Pred> bool Transport::waitFor(Pred pred) {
  for (int spin = 0; spin < 256; ++spin) {
    if (pred())
      return true;
    YieldProcessor();
  }
  HANDLE handles[2] = {m_doneEvent, m_hostProcess};
  for (;;) {
    m_header->clientSleeping.store(1);
    if (pred()) {
      m_header->clientSleeping.store(0);
      return true;
    }
    // The runtime's window code in the host sends messages to the game's
    // window (e.g. during Startup). Dispatch those cross-process sent
    // messages while blocked, or both processes wait on each other.
    DWORD r = MsgWaitForMultipleObjectsEx(2, handles, 5, QS_SENDMESSAGE, 0);
    if (r == WAIT_OBJECT_0 + 2) {
      MSG msg;
      PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
      continue;
    }
    if (r == WAIT_OBJECT_0 + 1 || m_header->hostState.load() >= static_cast<uint32_t>(HostState::Failed)) {
      if (!m_hostDead && m_log)
        m_log->write("bridge: host is gone: %s", m_header->lastError);
      m_hostDead = true;
      return false;
    }
  }
}

uint8_t *Transport::reserve(uint32_t size) {
  uint32_t phys = static_cast<uint32_t>(m_write % m_ringSize);
  if (phys + size > m_ringSize) {
    const uint32_t pad = m_ringSize - phys;
    if (!waitFor([&] { return m_write + pad - m_header->readPos.load(std::memory_order_acquire) <= m_ringSize; }))
      return nullptr;
    auto *rec = reinterpret_cast<Record *>(m_ring + phys);
    *rec = Record{Op::Pad, pad, 0};
    commit(pad);
    phys = 0;
  }
  if (!waitFor([&] { return m_write + size - m_header->readPos.load(std::memory_order_acquire) <= m_ringSize; }))
    return nullptr;
  return m_ring + phys;
}

void Transport::commit(uint32_t size) {
  m_write += size;
  m_header->writePos.store(m_write);
  if (m_header->hostSleeping.exchange(0))
    SetEvent(m_workEvent);
}

void Transport::writeRaw(Op op, const void *args, uint32_t argsSize, const void *payload, uint32_t payloadSize) {
  const uint32_t total = alignUp(sizeof(Record) + argsSize + payloadSize, kRecordAlign);
  uint8_t *dst = reserve(total);
  if (!dst)
    return;
  *reinterpret_cast<Record *>(dst) = Record{op, total, 0};
  std::memcpy(dst + sizeof(Record), args, argsSize);
  if (payloadSize)
    std::memcpy(dst + sizeof(Record) + argsSize, payload, payloadSize);
  commit(total);
}

uint64_t Transport::sendBlob(const void *data, size_t size) {
  const uint64_t id = m_nextBlob++;
  ArgsBlobBegin begin{id, size};
  writeRaw(Op::BlobBegin, &begin, sizeof(begin), nullptr, 0);
  const size_t chunk = m_ringSize / 8;
  for (size_t off = 0; off < size; off += chunk) {
    const size_t n = size - off < chunk ? size - off : chunk;
    ArgsBlobChunk args{id, off, n};
    writeRaw(Op::BlobChunk, &args, sizeof(args), static_cast<const uint8_t *>(data) + off, static_cast<uint32_t>(n));
  }
  return kPtrBlob | id;
}

uint64_t Transport::sendLarge(const void *data, size_t size) {
  const uint32_t total = alignUp(static_cast<uint32_t>(sizeof(Record) + size), kRecordAlign);
  if (size > m_ringSize / 8 || m_pendingData + total > m_ringSize / 4)
    return sendBlob(data, size);
  uint8_t *dst = reserve(total);
  if (!dst)
    return 0;
  const uint64_t payloadPos = m_write + sizeof(Record); // reserve() leaves m_write at dst
  *reinterpret_cast<Record *>(dst) = Record{Op::Data, total, 0};
  std::memcpy(dst + sizeof(Record), data, size);
  commit(total);
  m_pendingData += total;
  return kPtrRing | payloadPos;
}

uint64_t Transport::submit(std::vector<uint8_t> &record, bool sync) {
  const uint32_t size = alignUp(static_cast<uint32_t>(record.size()), kRecordAlign);
  record.resize(size);
  auto *rec = reinterpret_cast<Record *>(record.data());
  rec->size = size;
  rec->seq = sync ? ++m_seq : 0;
  if (size > m_ringSize / 4) {
    ArgsHandle args{sendBlob(record.data(), size) & ~kPtrTagMask};
    const uint32_t total = alignUp(sizeof(Record) + sizeof(args), kRecordAlign);
    uint8_t *dst = reserve(total);
    if (dst) {
      *reinterpret_cast<Record *>(dst) = Record{Op::Indirect, total, rec->seq};
      std::memcpy(dst + sizeof(Record), &args, sizeof(args));
      commit(total);
    }
  } else if (uint8_t *dst = reserve(size)) {
    std::memcpy(dst, record.data(), size);
    commit(size);
  }
  m_pendingData = 0;
  return rec->seq;
}

bool Transport::waitReply(uint64_t seq, const Reply *&reply, const uint8_t *&payload) {
  if (!waitFor([&] { return m_header->syncDone.load(std::memory_order_acquire) >= seq; }))
    return false;
  reply = reinterpret_cast<const Reply *>(m_reply);
  payload = m_reply + sizeof(Reply);
  return reply->seq == seq;
}

void Transport::throttleFrames(uint64_t submittedFrames, uint32_t maxQueued) {
  waitFor([&] { return submittedFrames - m_header->framesPresented.load(std::memory_order_acquire) < maxQueued; });
}

} // namespace bridge::client
