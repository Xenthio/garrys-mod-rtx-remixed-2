#include "texture_categories.h"

#include <windows.h>

#define REMIX_ALLOW_X86
#include <remix/remix_c.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace proxy::categories {
namespace {

// dxvk-remix texture list options and the instance category each implies.
// rtx.lightmapTextures and rtx.uiTextures select textures/screen UI rather
// than a category and are not listed.
const std::map<std::string, uint32_t> kOptions = {
    {"rtx.ignoreTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE},
    {"rtx.hideInstanceTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_HIDDEN},
    {"rtx.worldSpaceUiTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_WORLD_UI},
    {"rtx.worldSpaceUiBackgroundTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_WORLD_MATTE},
    {"rtx.skyBoxTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_SKY},
    {"rtx.postfx.motionBlurMaskOutTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_MOTION_BLUR},
    {"rtx.opacityMicromapIgnoreTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_OPACITY_MICROMAP},
    {"rtx.particleTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_PARTICLE},
    {"rtx.beamTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_BEAM},
    {"rtx.decalTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC},
    {"rtx.dynamicDecalTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_DYNAMIC},
    {"rtx.singleOffsetDecalTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_SINGLE_OFFSET},
    {"rtx.nonOffsetDecalTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_NO_OFFSET},
    {"rtx.cutoutTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_ALPHA_BLEND_TO_CUTOUT},
    {"rtx.terrainTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_TERRAIN},
    {"rtx.animatedWaterTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_ANIMATED_WATER},
    {"rtx.playerModelTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_THIRD_PERSON_PLAYER_MODEL},
    {"rtx.playerModelBodyTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_THIRD_PERSON_PLAYER_BODY},
    {"rtx.ignoreBakedLightingTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_BAKED_LIGHTING},
    {"rtx.ignoreAlphaOnTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_ALPHA_CHANNEL},
    {"rtx.ignoreTransparencyLayerTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_TRANSPARENCY_LAYER},
    {"rtx.particleEmitterTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_PARTICLE_EMITTER},
    {"rtx.smoothNormalsTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_SMOOTH_NORMALS},
};

std::shared_mutex g_lock;
std::map<std::string, std::vector<uint64_t>> g_lists;
std::unordered_map<uint64_t, uint32_t> g_flags;

// dxvk-remix hash lists: hex with or without 0x; a leading '-' is the
// two's complement of a signed 64-bit value.
std::vector<uint64_t> parse(const std::string &value) {
  std::vector<uint64_t> hashes;
  size_t start = 0;
  while (start <= value.size()) {
    size_t end = value.find(',', start);
    if (end == std::string::npos)
      end = value.size();
    std::string item = value.substr(start, end - start);
    start = end + 1;
    const size_t first = item.find_first_not_of(" \t\r\n"), last = item.find_last_not_of(" \t\r\n");
    if (first == std::string::npos)
      continue;
    item = item.substr(first, last - first + 1);
    const bool negative = item[0] == '-';
    if (negative)
      item.erase(0, 1);
    char *tail = nullptr;
    const uint64_t magnitude = std::strtoull(item.c_str(), &tail, 16);
    if (tail == item.c_str() || *tail)
      continue;
    hashes.push_back(negative ? ~magnitude + 1 : magnitude);
  }
  return hashes;
}

void rebuild() {
  g_flags.clear();
  for (const auto &[option, hashes] : g_lists) {
    const uint32_t flag = kOptions.at(option);
    for (const uint64_t hash : hashes)
      g_flags[hash] |= flag;
  }
}

void loadFile(const std::wstring &path) {
  FILE *file = _wfopen(path.c_str(), L"rb");
  if (!file)
    return;
  std::string text;
  char chunk[65536];
  for (size_t n; (n = fread(chunk, 1, sizeof chunk, file)) > 0;)
    text.append(chunk, n);
  fclose(file);
  for (size_t at = 0; at < text.size();) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos)
      end = text.size();
    const std::string line = text.substr(at, end - at);
    at = end + 1;
    const size_t eq = line.find('=');
    if (eq == std::string::npos || line.find('#') < eq)
      continue;
    std::string key = line.substr(0, eq);
    key.erase(key.find_last_not_of(" \t") + 1);
    key.erase(0, key.find_first_not_of(" \t"));
    if (kOptions.count(key))
      g_lists[key] = parse(line.substr(eq + 1));
  }
}

} // namespace

void load(const std::wstring &directory) {
  std::unique_lock lock(g_lock);
  g_lists.clear();
  loadFile(directory + L"\\rtx.conf");
  loadFile(directory + L"\\user.conf");
  rebuild();
}

bool set(const char *option, const char *value) {
  if (!option || !value || !kOptions.count(option))
    return false;
  std::unique_lock lock(g_lock);
  g_lists[option] = parse(value);
  rebuild();
  return true;
}

uint32_t lookup(uint64_t textureHash) {
  // Dev aid: OPENREMIX_PROXY_CATEGORY_MASK (hex) limits the categories applied.
  static const uint32_t mask = [] {
    char v[16] = {};
    return GetEnvironmentVariableA("OPENREMIX_PROXY_CATEGORY_MASK", v, sizeof(v)) ? uint32_t(strtoul(v, nullptr, 16))
                                                                                   : ~0u;
  }();
  if (!textureHash)
    return 0;
  std::shared_lock lock(g_lock);
  auto it = g_flags.find(textureHash);
  return it == g_flags.end() ? 0 : it->second & mask;
}

size_t size() {
  std::shared_lock lock(g_lock);
  return g_flags.size();
}

} // namespace proxy::categories
