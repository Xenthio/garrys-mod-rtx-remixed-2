#include "cubemap_overrides.h"
#include <windows.h>
#include <bcrypt.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace proxy::cubemaps {
  static_assert(sizeof(remixapi_ReflectionVolumeEXT) == 96);
  static_assert(sizeof(remixapi_ReflectionSamplingEXT) == 28);
  Fingerprint fingerprint(const std::vector<uint8_t> & bytes) {
    struct Hash {
      BCRYPT_ALG_HANDLE algorithm{};
      BCRYPT_HASH_HANDLE hash{};
      ~Hash() {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
      }
    } state;
    auto check = [](NTSTATUS status) { if (status < 0) throw std::runtime_error("BSP SHA-256 failed"); };
    check(BCryptOpenAlgorithmProvider(&state.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
    check(BCryptCreateHash(state.algorithm, &state.hash, nullptr, 0, nullptr, 0, 0));
    if (bytes.size() > UINT32_MAX) throw std::runtime_error("BSP too large for fingerprint");
    check(BCryptHashData(state.hash, const_cast<PUCHAR>(bytes.data()), ULONG(bytes.size()), 0));
    Fingerprint result{};
    check(BCryptFinishHash(state.hash, result.data(), ULONG(result.size()), 0));
    return result;
  }
  Document decode(const std::vector<uint8_t> & bytes, const Fingerprint &map) {
    constexpr size_t header = 44;
    const bool extended = bytes.size() >= header && !std::memcmp(bytes.data(), "BSPCUBE2", 8);
    if (bytes.size() < header || (!extended && std::memcmp(bytes.data(), "BSPCUBE1", 8)) ||
        std::memcmp(bytes.data()+8, map.data(), map.size()))
      throw std::runtime_error("Cubemap bounds belong to another BSP or have an invalid header");
    uint32_t count;
    std::memcpy(&count, bytes.data()+40, sizeof(count));
    const size_t stride = sizeof(remixapi_ReflectionVolumeEXT) + (extended ? sizeof(remixapi_ReflectionSamplingEXT) : 0);
    if (count > REMIXAPI_REFLECTION_VOLUMES_MAX_EXT || bytes.size() != header + size_t(count)*stride)
      throw std::runtime_error("Invalid cubemap bounds count or payload size");
    Document result;
    result.independentSampling = extended;
    result.volumes.resize(count);
    if (extended) result.sampling.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
      const auto record = bytes.data() + header + size_t(i)*stride;
      std::memcpy(&result.volumes[i], record, sizeof(result.volumes[i]));
      if (extended) std::memcpy(&result.sampling[i], record + sizeof(result.volumes[i]), sizeof(result.sampling[i]));
    }
    return result; // Native API validates IDs, finite rigid frames, bounds and origins atomically.
  }
  void Watcher::configure(const std::vector<uint8_t> & map, uint64_t sceneId, std::filesystem::path file) {
    m_file.clear();
    m_scene = 0;
    m_error.clear();
    m_map = fingerprint(map);
    m_scene = sceneId;
    m_file = std::move(file);
    m_seen.clear();
    m_applied = false;
    m_document = {};
    m_revision = 0;
    m_next = {};
  }
  void Watcher::poll(PFN_remixapi_SetReflectionVolumesEXT publish) {
    if (!publish || m_file.empty() || std::chrono::steady_clock::now() < m_next) return;
    m_next = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    try {
      std::error_code error;
      const bool exists = std::filesystem::exists(m_file, error);
      if (error) throw std::runtime_error("Cannot inspect cubemap sidecar");
      if (!exists) {
        if (m_applied && publish(nullptr) == REMIXAPI_ERROR_CODE_SUCCESS) {
          m_applied = false;
          m_document = {};
        }
        m_seen.clear();
        return;
      }
      struct File {
        HANDLE handle;
        ~File() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
      } input{CreateFileW(m_file.c_str(), GENERIC_READ,
          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
      if (input.handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open cubemap sidecar");
      LARGE_INTEGER length{};
      if (!GetFileSizeEx(input.handle, &length) || length.QuadPart < 0 ||
          length.QuadPart > 44 + 124*REMIXAPI_REFLECTION_VOLUMES_MAX_EXT)
        throw std::runtime_error("Cubemap sidecar exceeds runtime capacity");
      std::vector<uint8_t> bytes(static_cast<size_t>(length.QuadPart));
      DWORD read = 0;
      if (!ReadFile(input.handle, bytes.data(), DWORD(bytes.size()), &read, nullptr) || read != bytes.size())
        throw std::runtime_error("Incomplete cubemap sidecar read");
      if (bytes == m_seen) return;
      auto document = decode(bytes, m_map);
      const auto &volumes = document.volumes;
      const remixapi_ReflectionVolumesInfoEXT request{sizeof(request), REMIXAPI_REFLECTION_VOLUMES_VERSION_EXT,
          m_scene, ++m_revision, uint32_t(volumes.size()), 0, volumes.data()};
      auto result = REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
      if (document.independentSampling) {
        remixapi_ReflectionVolumesSamplingInfoEXT extended{request, document.sampling.data()};
        extended.base.structSize = sizeof(extended);
        extended.base.version = REMIXAPI_REFLECTION_VOLUMES_SAMPLING_VERSION_EXT;
        result = publish(&extended.base);
      } else result = publish(&request);
      if (result != REMIXAPI_ERROR_CODE_SUCCESS) throw std::runtime_error("Native renderer refused cubemap bounds");
      // Remember only accepted payloads: scene preparation can temporarily reject publication.
      // The next poll must retry unchanged bytes rather than silently losing the edit.
      m_seen = std::move(bytes);
      m_document = std::move(document);
      m_acceptedRevision = m_revision;
      m_applied = true;
      m_error.clear();
      std::fprintf(stderr, "Authored cubemap bounds: %zu volumes, revision %llu\n", m_document.volumes.size(),
                   static_cast<unsigned long long>(m_revision));
      if (m_document.volumes.empty())
        std::fprintf(stderr, "Local cubemaps disabled by empty authored set; remove the sidecar to restore automatic placement\n");
    } catch (const std::exception &error) {
      if (m_error != error.what()) std::fprintf(stderr, "Authored cubemap bounds: %s\n", error.what());
      m_error = error.what();
    }
  }
} // namespace proxy::cubemaps
