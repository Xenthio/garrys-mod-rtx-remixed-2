#pragma once
#include "source_lights.h"
#include <remix/remix_bake_ext.h>
#include <unordered_map>

namespace proxy::lighting {
// Immutable intensity-envelope definitions used only by the baker. Live style changes
// never touch these handles; each record points back to its live emitter.
class StyledLights {
public:
  remixapi_BakeStyledLightEXT capture(remixapi_Interface &api, const SourceLight &authored,
                                     uint64_t runtimeHash, uint32_t style,
                                     float lightScale = 1, float sunScale = 1, float currentStyle = 1);
  void reset(remixapi_Interface &api);

private:
  struct Entry {
    SourceLight source{};
    float lightScale = 0, sunScale = 0, envelope = 0;
    remixapi_LightHandle handle = nullptr;
  };
  std::unordered_map<uint64_t, Entry> m_lights;
};
} // namespace proxy::lighting
