#pragma once
#include "cubemap_overrides.h"
#include <charconv>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>

namespace proxy::cubemaps {
  using CaptureOrigin = std::array<int32_t, 3>;

  inline bool supportsReceiverBindings(PFN_remixapi_GetReflectionBindingVersionEXT query) {
    return query && query() >= REMIXAPI_REFLECTION_BINDING_VERSION_EXT;
  }

  // Source's compiled $envmap value names a capture, independently of the
  // material filename. This deliberately does not invent influence or bounds.
  inline std::optional<CaptureOrigin> captureOrigin(std::string_view texture, std::string_view map) {
    if (texture.size() > 4096 || map.empty() || map.size() > 256) return std::nullopt;
    auto normalized = [](std::string_view input) {
      std::string result;
      for (char c : input) {
        if (c == '\\') c = '/';
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != '/' || result.empty() || result.back() != '/') result.push_back(c);
      }
      return result;
    };
    const auto name = normalized(texture);
    const auto mapPath = normalized(map);
    const auto guarded = "/" + mapPath + "/";
    if (mapPath.front() == '/' || mapPath.find(':') != std::string::npos ||
        mapPath.find('\0') != std::string::npos || guarded.find("/../") != std::string::npos ||
        guarded.find("/./") != std::string::npos) return std::nullopt;
    const auto prefix = "maps/" + mapPath + "/c";
    if (name.compare(0, prefix.size(), prefix) != 0) return std::nullopt;
    std::string_view numbers(name.data() + prefix.size(), name.size() - prefix.size());
    CaptureOrigin origin{};
    for (size_t axis = 0; axis < origin.size(); ++axis) {
      const auto separator = numbers.find('_');
      const auto component = numbers.substr(0, separator);
      if (component.empty()) return std::nullopt;
      const auto parsed = std::from_chars(component.data(), component.data() + component.size(), origin[axis]);
      if (parsed.ec != std::errc{} || parsed.ptr != component.data() + component.size()) return std::nullopt;
      if ((axis < 2) != (separator != std::string_view::npos)) return std::nullopt;
      if (separator != std::string_view::npos) numbers.remove_prefix(separator + 1);
    }
    return origin;
  }

  // An exact, unique capture match is evidence of identity, not proximity.
  // Use double comparisons so large integer names cannot alias rounded floats.
  inline uint64_t uniqueVolume(const CaptureOrigin &origin, const Document &document) {
    uint64_t found = 0;
    for (const auto &volume : document.volumes) {
      bool same = true;
      for (size_t axis = 0; axis < origin.size(); ++axis)
        same &= std::isfinite(volume.capture[axis]) && double(volume.capture[axis]) == double(origin[axis]);
      if (!same) continue;
      if (found || !volume.id) return 0;
      found = volume.id;
    }
    return found;
  }
}
