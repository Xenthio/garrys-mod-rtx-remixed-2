#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

// Lights read where the engine keeps them, independent of what is drawn or
// in view: lightstyle values (switchable/flickering map lights), active
// dlights, projected textures (lamps, flashlights). Interfaces only: no code
// patches, nothing a server can query.
//
// Known limit: entity-driven lights (gmod_light, lamps) stop when their
// entity goes dormant outside the server's PVS for the player; the client
// then stops making them, and holding the last state would be stale.
namespace proxy::engine_lights {

// Resolves the engine interfaces and installs the shadow manager hooks
// (main thread, engine.dll loaded). False when unavailable.
bool install();

// Once per main view on the main thread (the DrawWorldLists hook): samples
// the engine's light state for the renderer.
void sample();

// A light in the engine's LightDesc_t terms, as bsp::Light (type 1 point,
// 3 spot; color in Source's linear intensity units). hash: a stable identity
// (the definition changes under it).
struct Light {
  uint64_t hash = 0;
  int type = 0;
  float color[3] = {}, position[3] = {}, direction[3] = {};
  float attenuation[3] = {}; // constant, linear, quadratic
  float thetaDot = 0, phiDot = 0;
};
struct Snapshot {
  bool styles = false;  // style[] was sampled
  float style[128] = {}; // IVEngineClient::LightStyleValue (1 normal, 0 off)
  std::vector<Light> dlights, projected;
};

// Whether lightstyle values are sampled: the BSP world then submits the
// map's styled lights itself (decided when a map loads).
bool stylesActive();

// Render thread. sample() announces each changed snapshot with the marker
// "openremix:lights <sequence>", which travels with the frame's draws
// (queued material system): activate() makes that snapshot the current one.
void activate(uint32_t sequence);
// The snapshot of the frame being presented (null before the first).
std::shared_ptr<const Snapshot> current();
// What the renderer made of a frame (statistics): styled map lights and how
// many of them are off, SetLight lights dropped as copies of a dlight.
void report(uint32_t styled, uint32_t styledOff, uint32_t duplicates);

void dumpStatistics(FILE *out);

} // namespace proxy::engine_lights
