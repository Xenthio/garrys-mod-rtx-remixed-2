#pragma once

#include <cstdint>
#include <cstdio>

// Portal 2 (x86): what only Portal 2 has, beside the x86 engine side it
// shares with L4D2 (x86_engine.h).
//
//  * portal2_portals.cpp: the portals (prop_portal) as path-traced ray
//    portals. Each activated portal is a Remix instance: an oval mesh in the
//    portal's frame whose material carries remixapi_MaterialInfoPortalEXT
//    (rayPortalIndex; the partner is index ^ 1). The runtime teleports the
//    rays that meet it through the pair's transform.
//    Around each, the game's own portal effects, drawn by the runtime: the rim
//    (PortalRefract $Stage 2, remixapi_MaterialPortalRimEXT: the glow at the
//    opening's edge and a lone portal's swirl) and the ghost
//    (PortalStaticOverlay $ghostoverlay: the portal's outline and corner
//    brackets seen through what hides it). The engine's own draws of both go.
//  * remix_output.cpp (PROXY_GAME_PORTAL2): the draws the engine makes inside
//    its own portal views are kept as instances (deduplicated against the
//    main view's): what only those views show (the player's own model,
//    reflection-only; models on the far side) is in the scene. The engine's
//    portal surfaces (models/portals/...) are no opaque instances.
//
// OPENREMIX_PROXY_PORTALS=0: no portal instances, the engine's portal
// surfaces and views as before. OPENREMIX_PROXY_PORTAL_VIEWS=0: portal
// instances, but the portal views' draws left out (as before).
struct remixapi_Interface;

namespace proxy::portal2 {

// The two switches above (read once).
bool enabled();
bool viewsKept();

// client.dll loaded (x86_engine's install, main thread): the portal class,
// its networked variables. False when unavailable (nothing is submitted).
bool install();

// Main thread, once per frame (x86_engine's mainFrame): reads the portals,
// assigns their ray portal indices and announces a changed set with the
// marker "openremix:portals <sequence>", which travels with the frame's draws.
void sample();

// Render thread. The marker: makes that set the current one.
void activate(uint32_t sequence);
// While the frame is submitted (remix_output's onPresent): the current set's
// instances (meshes and materials made as needed). eye: the main view's
// camera position (the ghosts fade out near a portal's front), or null.
void submit(const remixapi_Interface &api, const float *eye);
// The map changes (remix_output's "map" marker): the meshes go.
void reset(const remixapi_Interface &api);

// Render thread. A SolidEnergy surface the runtime evaluates
// (remixapi_MaterialSolidEnergyEXT): the material for the engine's material
// `name` (only effects/projected_wall, the light bridge, is known), with the
// pixel shader's output intensity (c6.w) as the draw has it; null for any
// other, or when OPENREMIX_PROXY_SOLID_ENERGY=0 (the engine's overlay then).
// Its textures are read from the game's VPKs once.
remixapi_MaterialHandle solidEnergyMaterial(const remixapi_Interface &api, const char *name, float outputIntensity);
// The light a bridge gives, per unit of what it adds where seen
// (OPENREMIX_PROXY_SOLID_ENERGY_LIGHT, default 1: as bright as it looks; 0:
// no light).
float solidEnergyLight();

// A material of the engine's portal surfaces ("models/portals/...": the
// stencil hole, the portal's render-target mesh, its static overlays).
bool isPortalMaterial(const char *name);
// Whether the proxy draws the rims (OPENREMIX_PROXY_PORTAL_RIM > 0, the
// default 1: the light its rims give): the engine's blended portal overlays
// (PortalRefract's rim, PortalStaticOverlay's ghosts) are then dropped. 0:
// no rims of the proxy's, the engine's overlays raster overlays as before.
bool ownRims();
// While ownRims(): whether the engine's rims are replayed on the path-traced
// frame with its own shaders instead of the runtime's rim overlay, as host
// passes the runtime runs whatever its "Pass-through game shaders" setting
// (hostpass::recordAlways; drawn over the frame, depth-tested against the
// runtime's depth); the proxy's rim then only lights the scene and shows in
// reflections. OPENREMIX_PROXY_PORTAL_ENGINE_RIMS=1; off by default (the
// replay flickered).
bool engineRims();
// Their depth test: the runtime's depth pushed back by this factor (they lie
// on the portal's wall). OPENREMIX_PROXY_PORTAL_RIM_DEPTH, default 1.01 (a
// unit at 100), at least 1.
float engineRimDepthScale();

void dumpStatistics(FILE *out);

} // namespace proxy::portal2
