#include "styled_lights.h"
#include <cstdio>
#include <cstdlib>

namespace {
unsigned creates = 0, destroys = 0;
float power = 0;
void require(bool ok, const char *message) {
  if (!ok) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
remixapi_ErrorCode REMIXAPI_CALL create(const remixapi_LightInfo *info, remixapi_LightHandle *out) {
  ++creates;
  power = info->radiance.x;
  *out = reinterpret_cast<remixapi_LightHandle>(uintptr_t(info->hash));
  return REMIXAPI_ERROR_CODE_SUCCESS;
}
remixapi_ErrorCode REMIXAPI_CALL destroy(remixapi_LightHandle) {
  ++destroys;
  return REMIXAPI_ERROR_CODE_SUCCESS;
}
}
int main() {
  remixapi_Interface api{};
  api.CreateLight = create;
  api.DestroyLight = destroy;
  proxy::lighting::StyledLights lights;
  proxy::lighting::SourceLight source{};
  source.type = 1;
  source.color[0] = 4;
  source.attenuation2 = 1;
  const auto a = lights.capture(api, source, 123, 32);
  require(a.authoredLight && a.authoredLight != a.runtimeLight && uintptr_t(a.runtimeLight) == 123 &&
          a.style == 32 && creates == 1 && power > 0, "Authored full-on light has a separate bake-only handle");
  const auto b = lights.capture(api, source, 123, 32);
  require(b.authoredLight == a.authoredLight && creates == 1,
          "Repeated capture is independent of live on/off/flicker state and reuses its definition");
  source.position[0] += 8;
  lights.capture(api, source, 123, 32);
  require(creates == 2, "Authored geometry edits replace the bake definition");
  lights.capture(api, source, 123, 32, 2);
  require(creates == 3, "Authored conversion scale edits replace the bake definition");
  require(!lights.capture(api, source, 123, 0).authoredLight &&
          !lights.capture(api, source, 123, 128).authoredLight && creates == 3,
          "Only nonzero valid Source styles enter styled capture");
  lights.capture(api, source, 123, 32, 2, 1, 3);
  require(creates == 3, "Boosted styles within the envelope do not restart baking");
  lights.capture(api, source, 123, 32, 2, 1, 9);
  require(creates == 4, "Exceeding the envelope expands authored range once");
  lights.capture(api, source, 123, 32, 2, 1, 0);
  require(creates == 4, "Switch-off retains the expanded envelope instead of repeatedly rebaking");
  lights.reset(api);
  require(destroys == 1, "Map reset releases canonical handles exactly once");
  lights.reset(api);
  require(destroys == 1, "Repeated reset is inert");
  std::puts("Immutable styled-light capture, identity and reset passed");
}
