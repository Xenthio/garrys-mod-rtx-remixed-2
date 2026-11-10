#include "cubemap_overrides.h"
#include <cstring>
#include <fstream>
#include <cstdio>
#include <thread>
#include <stdexcept>
using namespace proxy::cubemaps;
static_assert(REMIXAPI_REFLECTION_VOLUMES_MAX_EXT >= 64, "Use the matching OpenRemix SDK for 64-volume authoring");
namespace {
  unsigned calls = 0, clears = 0, volumeCount = 0;
  uint64_t revision = 0;
  int32_t priority = 0;
  bool refuseNext = false;
  uint32_t version = 0;
  float influenceMinimum = 0, blendDistance = 0;
  void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
  remixapi_ErrorCode REMIXAPI_CALL publish(const remixapi_ReflectionVolumesInfoEXT *request) {
    if (!request) { ++clears; return REMIXAPI_ERROR_CODE_SUCCESS; }
    require(request->sceneId == 42 && request->volumeCount <= 1, "Watcher preserves scene scope and bounded count");
    ++calls; revision = request->revision; volumeCount = request->volumeCount;
    if (refuseNext) { refuseNext = false; return REMIXAPI_ERROR_CODE_GENERAL_FAILURE; }
    if (volumeCount) priority = request->volumes[0].priority;
    version = request->version;
    if (version == REMIXAPI_REFLECTION_VOLUMES_SAMPLING_VERSION_EXT) {
      require(request->structSize == sizeof(remixapi_ReflectionVolumesSamplingInfoEXT), "Sampling request preserves its extended size");
      const auto extended = reinterpret_cast<const remixapi_ReflectionVolumesSamplingInfoEXT *>(request);
      require(extended->sampling && volumeCount == 1, "Extended watcher passes matching sampling records");
      influenceMinimum = extended->sampling[0].minimum[0];
      blendDistance = extended->sampling[0].blendDistance;
    }
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }
}
int main(int argc, char **argv) try {
  require(argc == 2, "Expected isolated output path");
  const std::vector<uint8_t> map{'a','b','c'};
  const auto hash = fingerprint(map);
  const Fingerprint expected{0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
      0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
  require(hash == expected, "Adapter SHA-256 matches the editor fingerprint algorithm");
  std::vector<uint8_t> bytes(44+sizeof(remixapi_ReflectionVolumeEXT));
  std::memcpy(bytes.data(), "BSPCUBE1", 8);
  std::memcpy(bytes.data()+8, hash.data(), hash.size());
  const uint32_t count = 1; std::memcpy(bytes.data()+40, &count, 4);
  remixapi_ReflectionVolumeEXT volume{}; volume.id = 19; volume.priority = -7;
  std::memcpy(bytes.data()+44, &volume, sizeof(volume));
  require(decode(bytes, hash).volumes.front().priority == -7, "Packed sidecar preserves signed priorities");
  auto full = bytes;
  const uint32_t maximum = REMIXAPI_REFLECTION_VOLUMES_MAX_EXT;
  full.resize(44 + size_t(maximum) * sizeof(volume));
  std::memcpy(full.data()+40, &maximum, 4);
  for (uint32_t i = 0; i < maximum; ++i) {
    auto entry = volume; entry.id = i + 1;
    std::memcpy(full.data()+44+size_t(i)*sizeof(entry), &entry, sizeof(entry));
  }
  require(decode(full, hash).volumes.size() == maximum, "The complete authored volume quota decodes without truncation");
  auto bad = hash; bad[0] ^= 1;
  bool rejected = false;
  try { decode(bytes, bad); } catch (const std::exception &) { rejected = true; }
  require(rejected, "Another BSP revision cannot pass fingerprint validation");
  auto oversized = bytes; const uint32_t excessive = REMIXAPI_REFLECTION_VOLUMES_MAX_EXT + 1; std::memcpy(oversized.data()+40, &excessive, 4);
  rejected = false;
  try { decode(oversized, hash); } catch (const std::exception &) { rejected = true; }
  require(rejected, "Oversized volume arrays are refused before allocation");
  const auto file = std::filesystem::path(argv[1]) / "test.cubemaps.bin";
  std::filesystem::create_directories(file.parent_path());
  auto write = [&] { std::ofstream output(file, std::ios::binary); output.write(reinterpret_cast<const char *>(bytes.data()), bytes.size()); };
  write();
  Watcher watcher; watcher.configure(map, 42, file); watcher.poll(publish);
  require(calls == 1 && revision == 1 && priority == -7, "Initial sidecar is published once");
  require(watcher.accepted() && watcher.accepted()->volumes[0].priority == -7,
          "Receiver lookup sees only a renderer-accepted document");
  require(watcher.acceptedRevision() == 1, "Accepted metadata carries the successful request revision");
  volume.priority = 9; std::memcpy(bytes.data()+44, &volume, sizeof(volume)); write();
  std::this_thread::sleep_for(std::chrono::milliseconds(510)); watcher.poll(publish);
  require(calls == 2 && revision == 2 && priority == 9, "Changed sidecars publish independently of bake membership");
  std::this_thread::sleep_for(std::chrono::milliseconds(510)); watcher.poll(publish);
  require(calls == 2, "Unchanged sidecars do not restart reflection work");
  bytes[8] ^= 1; write();
  std::this_thread::sleep_for(std::chrono::milliseconds(510)); watcher.poll(publish);
  require(calls == 2 && clears == 0, "Invalid edits preserve the previously valid override");
  require(watcher.accepted() && watcher.accepted()->volumes[0].priority == 9,
          "Invalid input cannot replace accepted receiver identity metadata");
  bytes[8] ^= 1; // Restore the valid BSP fingerprint before publishing an empty authored set.
  bytes.resize(44);
  const uint32_t empty = 0; std::memcpy(bytes.data()+40, &empty, sizeof(empty)); write();
  std::this_thread::sleep_for(std::chrono::milliseconds(510)); watcher.poll(publish);
  require(calls == 3 && revision == 3 && volumeCount == 0 && clears == 0,
          "A saved empty authored set explicitly disables local cubemaps rather than restoring automatic placement");
  std::filesystem::remove(file);
  std::this_thread::sleep_for(std::chrono::milliseconds(510)); watcher.poll(publish);
  require(clears == 1, "Removing the sidecar restores automatic cubemaps");
  require(!watcher.accepted(), "Successful removal clears receiver identity metadata");
  write();
  refuseNext = true;
  std::this_thread::sleep_for(std::chrono::milliseconds(510)); watcher.poll(publish);
  require(calls == 4, "A recreated sidecar reaches the renderer");
  require(!watcher.accepted(), "Refused publication cannot activate receiver metadata");
  require(watcher.acceptedRevision() == 0, "Refused publication does not advance accepted receiver state");
  std::this_thread::sleep_for(std::chrono::milliseconds(510)); watcher.poll(publish);
  require(calls == 5 && revision == 5, "Unchanged bytes retry after renderer rejection");
  std::this_thread::sleep_for(std::chrono::milliseconds(510)); watcher.poll(publish);
  require(calls == 5, "Accepted retry stops redundant publication");
  bytes.resize(44 + 124);
  std::memcpy(bytes.data(), "BSPCUBE2", 8);
  std::memcpy(bytes.data()+40, &count, 4);
  std::memcpy(bytes.data()+44, &volume, sizeof(volume));
  remixapi_ReflectionSamplingEXT sampling{};
  sampling.minimum[0] = -2;
  sampling.maximum[0] = 3;
  sampling.blendDistance = .25f;
  std::memcpy(bytes.data()+44+96, &sampling, sizeof(sampling));
  const auto extended = decode(bytes, hash);
  require(extended.independentSampling && extended.volumes.size() == 1 && extended.sampling.size() == 1 &&
              extended.sampling[0].minimum[0] == -2 && extended.sampling[0].blendDistance == .25f,
          "Version-two sidecars preserve independent sampling alongside unchanged volume records");
  auto truncated = bytes; truncated.pop_back();
  rejected = false;
  try { decode(truncated, hash); } catch (const std::exception &) { rejected = true; }
  require(rejected, "Truncated version-two sampling cannot publish a partial edit");
  write();
  refuseNext = true;
  std::this_thread::sleep_for(std::chrono::milliseconds(510)); watcher.poll(publish);
  require(calls == 6, "Version-two publication reaches the renderer and preserves rejection semantics");
  std::this_thread::sleep_for(std::chrono::milliseconds(510)); watcher.poll(publish);
  require(calls == 7 && version == REMIXAPI_REFLECTION_VOLUMES_SAMPLING_VERSION_EXT &&
              influenceMinimum == -2 && blendDistance == .25f,
          "An unchanged version-two document retries and publishes independent sampling atomically");
  require(watcher.accepted() && watcher.accepted()->independentSampling &&
              watcher.accepted()->sampling[0].blendDistance == .25f,
          "Accepted metadata retains version-two sampling without inventing influence bounds");
  std::filesystem::remove(file);
  std::puts("Cubemap sidecar fingerprints, bounded decoding and live publication passed");
} catch (const std::exception &error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
