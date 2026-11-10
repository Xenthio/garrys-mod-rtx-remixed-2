#include "material_parameters.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

using proxy::engine_materials::ParameterNames;
void require(bool condition, const char *message) {
  if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
int main() {
  ParameterNames names;
  std::vector<const char *> layout{"$color", "$alpha", "$ALPHA", "$detail"};
  uint32_t reads = 0;
  auto at = [&](uint16_t index) { ++reads; return layout[index]; };
  require(names.find("$alpha", uint16_t(layout.size()), at) == std::vector<uint16_t>{1, 2},
          "Duplicate names retain source order independently of their live value types");
  const auto coldReads = reads;
  require(names.find("$ALPHA", uint16_t(layout.size()), at) == std::vector<uint16_t>{1, 2} && reads == coldReads,
          "Case-insensitive repeated queries reuse the resolved indices");
  require(names.find("$missing", uint16_t(layout.size()), at).empty(), "Missing parameters return no indices");
  const auto missingReads = reads;
  require(names.find("$MISSING", uint16_t(layout.size()), at).empty() && reads == missingReads,
          "A complete layout can reuse a negative name lookup");

  layout = {"$alpha", "$color"};
  require(names.find("$alpha", uint16_t(layout.size()), at) == std::vector<uint16_t>{0},
          "A changed parameter count cannot reuse old indices");
  layout = {"$color", "$alpha"};
  names = {}; // The same-count shader/layout replacement resets the owning Entry.
  require(names.find("$alpha", uint16_t(layout.size()), at) == std::vector<uint16_t>{1},
          "An owning layout replacement discards same-count lookup results");

  names = {};
  layout = {nullptr, "$color", "$alpha"};
  require(names.find("$alpha", uint16_t(layout.size()), at) == std::vector<uint16_t>{2},
          "An unreadable variable does not hide a later readable match");
  require(names.find("$later", uint16_t(layout.size()), at).empty(), "An incomplete layout can report a current miss");
  layout[0] = "$later";
  require(names.find("$later", uint16_t(layout.size()), at) == std::vector<uint16_t>{0},
          "Temporarily unreadable names never poison a later lookup");
  names = {};
  layout = {nullptr, "$alpha"};
  require(names.find("$alpha", uint16_t(layout.size()), at) == std::vector<uint16_t>{1},
          "A partial positive result remains usable");
  layout[0] = "$alpha";
  require(names.find("$alpha", uint16_t(layout.size()), at) == std::vector<uint16_t>{0, 1},
          "A formerly unreadable earlier duplicate retains first-match ownership");
  std::puts("Material parameter names, misses and layout replacement passed");
}
