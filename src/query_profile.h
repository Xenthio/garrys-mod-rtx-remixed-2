#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>

namespace proxy::d3d {

// Diagnostic interval counters only. They describe calls into the driver,
// not the caller's sleep between polls or the lifetime of a GPU query.
class QueryProfile {
public:
  enum Kind { Event, Occlusion, Other, Count };

  void issued(Kind kind) { m_counters[kind].issued.fetch_add(1, std::memory_order_relaxed); }

  void polled(Kind kind, bool pending, bool failed, bool flush, uint64_t microseconds) {
    auto &c = m_counters[kind];
    c.calls.fetch_add(1, std::memory_order_relaxed);
    c.pending.fetch_add(pending, std::memory_order_relaxed);
    c.failed.fetch_add(failed, std::memory_order_relaxed);
    c.flush.fetch_add(flush, std::memory_order_relaxed);
    c.microseconds.fetch_add(microseconds, std::memory_order_relaxed);
  }

  void dump(FILE *out, double frames) {
    constexpr const char *names[] = {"event", "occlusion", "other"};
    for (unsigned i = 0; i < Count; ++i) {
      auto &c = m_counters[i];
      // Concurrent calls can straddle interval boundaries. These are aggregate
      // rates, not a transactional trace of individual query objects.
      std::fprintf(out, "   query profile %s/frame: issued %.2f polls %.2f pending %.2f failed %.2f flush %.2f GetData %.4f ms (excludes caller sleeps)\n",
                   names[i], c.issued.exchange(0) / frames, c.calls.exchange(0) / frames,
                   c.pending.exchange(0) / frames, c.failed.exchange(0) / frames,
                   c.flush.exchange(0) / frames, c.microseconds.exchange(0) / frames / 1000.0);
    }
  }

private:
  struct Counters {
    std::atomic<uint64_t> issued{0}, calls{0}, pending{0}, failed{0}, flush{0}, microseconds{0};
  };
  std::array<Counters, Count> m_counters;
};

} // namespace proxy::d3d
