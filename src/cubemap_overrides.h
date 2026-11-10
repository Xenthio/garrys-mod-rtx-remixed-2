#pragma once
#include <remix/remix_reflection_ext.h>
#include <array>
#include <chrono>
#include <filesystem>
#include <vector>
namespace proxy::cubemaps {
  using Fingerprint = std::array<uint8_t, 32>;
  Fingerprint fingerprint(const std::vector<uint8_t> & bytes);
  struct Document {
    bool independentSampling = false;
    std::vector<remixapi_ReflectionVolumeEXT> volumes;
    std::vector<remixapi_ReflectionSamplingEXT> sampling;
  };
  Document decode(const std::vector<uint8_t> & bytes, const Fingerprint &map);
  class Watcher {
  public:
    void configure(const std::vector<uint8_t> & map, uint64_t sceneId, std::filesystem::path file);
    void poll(PFN_remixapi_SetReflectionVolumesEXT publish);
    // Render-thread view, valid until the next poll/configure. This records API
    // acceptance, not completion of the renderer's asynchronous capture work.
    const Document *accepted() const { return m_applied ? &m_document : nullptr; }
    uint64_t acceptedRevision() const { return m_applied ? m_acceptedRevision : 0; }
  private:
    Fingerprint m_map{};
    std::filesystem::path m_file;
    uint64_t m_scene = 0, m_revision = 0;
    uint64_t m_acceptedRevision = 0;
    bool m_applied = false;
    Document m_document;
    std::string m_error;
    std::vector<uint8_t> m_seen;
    std::chrono::steady_clock::time_point m_next{};
  };
} // namespace proxy::cubemaps
