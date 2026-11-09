#pragma once
#include "common/platform.h"
#include "common/protocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bridge::client {

// Single-producer end of the shared-memory ring. All methods except
// `start` require the caller to hold the client API lock.
class Transport {
public:
  // Launches the x64 host and waits until it has loaded the runtime.
  int32_t start(Log &log, std::string &error);
  void stop();
  bool running() const { return m_header != nullptr && !m_hostDead; }

  // Writes one complete record (Record header included, already sized).
  // Records larger than a quarter of the ring travel as a blob plus an
  // Indirect record. Returns the record's sequence number when `sync`.
  uint64_t submit(std::vector<uint8_t> &record, bool sync);

  // Stores a large payload outside the current record and returns an
  // encoded pointer (kPtrRing or kPtrBlob) valid for the next record.
  uint64_t sendLarge(const void *data, size_t size);

  // Waits for the host to complete synchronous command `seq`.
  // Returns false if the host died.
  bool waitReply(uint64_t seq, const proto::Reply *&reply, const uint8_t *&payload);

  // Blocks while `frames` Present commands are still queued in the host.
  void throttleFrames(uint64_t submittedFrames, uint32_t maxQueued);

  // A DATA payload or blob awaits the next record (it is not self-contained).
  bool pendingData() const { return m_pendingData != 0; }

  const proto::Header *header() const { return m_header; }
  const char *hostError() const { return m_header ? m_header->lastError : ""; }
  uint32_t asyncErrors() const { return m_header ? m_header->asyncErrors.load() : 0; }

private:
  uint8_t *reserve(uint32_t size);
  void commit(uint32_t size);
  template <class Pred> bool waitFor(Pred pred);
  void writeRaw(proto::Op op, const void *args, uint32_t argsSize, const void *payload, uint32_t payloadSize);
  uint64_t sendBlob(const void *data, size_t size);

  HANDLE m_mapping = nullptr;
  HANDLE m_workEvent = nullptr;
  HANDLE m_doneEvent = nullptr;
  HANDLE m_hostProcess = nullptr;
  HANDLE m_job = nullptr;
  proto::Header *m_header = nullptr;
  uint8_t *m_reply = nullptr;
  uint8_t *m_ring = nullptr;
  uint32_t m_ringSize = 0;
  uint64_t m_write = 0;
  uint64_t m_seq = 0;
  uint64_t m_nextBlob = 1;
  uint64_t m_pendingData = 0; // DATA bytes the next record will reference
  bool m_hostDead = false;
  Log *m_log = nullptr;
};

} // namespace bridge::client
