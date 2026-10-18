#include "styled_lights.h"
#include <cstring>
#include <algorithm>
#include <cmath>

namespace proxy::lighting {
remixapi_BakeStyledLightEXT StyledLights::capture(remixapi_Interface &api, const SourceLight &authored,
                                                uint64_t runtimeHash, uint32_t style,
                                                float lightScale, float sunScale, float currentStyle) {
  if (!runtimeHash || !style || style > 127 || authored.type == 2)
    return {};
  if (!std::isfinite(currentStyle) || currentStyle < 0) return {};
  auto &entry = m_lights[runtimeHash];
  // Reserve a stable range envelope for ordinary boosted styles. Only an
  // intensity above the retained envelope requires a new authored bake input.
  const float envelope = std::max(entry.envelope, std::exp2(std::ceil(std::log2(std::max(4.f, currentStyle)))));
  if (!entry.handle || std::memcmp(&entry.source, &authored, sizeof(authored)) ||
      entry.lightScale != lightScale || entry.sunScale != sunScale || entry.envelope != envelope) {
    // Use a separate stable identity from this live light. BSP source hashes are odd;
    // the even xor salt preserves that and cannot produce zero for those IDs.
    uint64_t hash = runtimeHash ^ 0x7374796c6562616aull;
    if (!hash) hash = ~runtimeHash;
    auto maximum = authored;
    for (auto &value : maximum.color) value *= envelope;
    const auto handle = createLight(api, maximum, hash, lightScale, sunScale);
    if (!handle) return {}; // retry without replacing the completed definition
    entry = {authored, lightScale, sunScale, envelope, handle};
  }
  return {entry.handle, reinterpret_cast<remixapi_LightHandle>(uintptr_t(runtimeHash)), style, 0};
}
void StyledLights::reset(remixapi_Interface &api) {
  if (api.DestroyLight)
    for (const auto &[id, entry] : m_lights)
      if (entry.handle) api.DestroyLight(entry.handle);
  m_lights.clear();
}
} // namespace proxy::lighting
