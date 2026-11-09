// The GMod-only modules' entry points for the builds that do not have them
// (Team Fortress 2, Left 4 Dead 2, Half-Life 2 RTX, Entropy : Zero 2, Portal
// 2): engine_patches, engine_nocull, engine_shadows, engine_mirrors and
// engine_lights patch one build of GMod's x64 engine.dll and client.dll. Each
// is off here, as it is on GMod when it does not validate; Portal 2 has
// engine_lights' projected textures (its own engine slots, engine_lights.cpp),
// Entropy : Zero 2 its own engine_lights (ez2_engine.cpp), L4D2 its own
// flashlights instead.
//
// TF2 shares GMod's engine_hooks (the world-list and map markers through
// engine interfaces, the slots per game.h) and its in-process panel
// (remix_ui).
//
// L4D2 and Portal 2 have their own engine side (x86_engine.h):
// engine::install (x86_engine.cpp: world lists, overlays, static props, the
// map, cvars, anti-culling; L4D2's flashlights, l4d2.h). Half-Life 2 RTX has
// none yet (hl2rtx_engine.cpp). All three have remix_ui (bridge_ui.cpp: the
// runtime's UI in the bridge host, F12).
#include "engine_lights.h"
#include "engine_nocull.h"
#include "engine_patches.h"
#include "engine_mirrors.h"
#include "engine_shadows.h"

namespace proxy::engine_patches {
bool install() { return false; }
void beginWorldLists() {}
void endWorldLists() {}
void dumpStatistics(FILE *) {}
} // namespace proxy::engine_patches

namespace proxy::engine_nocull {
bool install(void *, void *) { return false; }
void dumpStatistics(FILE *) {}
} // namespace proxy::engine_nocull

namespace proxy::engine_shadows {
bool install() { return false; }
void dumpStatistics(FILE *) {}
} // namespace proxy::engine_shadows

namespace proxy::engine_mirrors {
void install() {}
void dumpStatistics(FILE *) {}
} // namespace proxy::engine_mirrors

// Portal 2 and Entropy : Zero 2 build engine_lights themselves.
#if !defined(PROXY_GAME_PORTAL2) && !defined(PROXY_GAME_EZ2)
namespace proxy::engine_lights {
bool install() { return false; }
void sample() {}
// The BSP world leaves the map's styled lights to the draws' SetLights.
bool stylesActive() { return false; }
void activate(uint32_t) {}
std::shared_ptr<const Snapshot> current() { return nullptr; }
void report(uint32_t, uint32_t, uint32_t) {}
void dumpStatistics(FILE *) {}
} // namespace proxy::engine_lights
#endif

