#pragma once
// Host passes: the engine's screen-space passes (post-processing effects on
// render-target textures and the frame: bloom, colour modification, motion
// blur, sharpen, addon shaders) recorded from the D3D9 stream and replayed by
// the runtime on its own frame with the game's own shaders, compiled to
// SPIR-V by the vendored DXVK dxso (remixapi_HostPassEXT). Render thread.
#include <cstdint>
#include <cstdio>
#include <d3d9.h>

namespace proxy::remix {
struct Draw;
}
namespace proxy::hostpass {
bool enabled();
// Whether a draw is one to record: into a render-target texture, or sampling
// one (the caller checks that it is flat, not a view).
bool wants(const proxy::remix::Draw &d);
// Records the draw; false when it cannot be (a plain texture sampled, no
// shader code): the caller draws it as it would have.
// `projection`: the draw's D3D projection (its depth test, when it has one, is
// made against the runtime's depth through it).
bool recordDraw(const proxy::remix::Draw &d, const float projection[16]);
// A draw the runtime replays whatever the user's host-pass setting
// (REMIXAPI_HOST_PASS_ALWAYS_EXT): an engine overlay that belongs on the
// path-traced frame (Portal 2's portal rims), recorded as recordDraw does
// even while enabled() is false. It may sample plain textures only (they go up
// as host targets), not render-target textures. `depthScale`: for its depth
// test the runtime's depth is pushed back by this factor (an overlay lying on
// a surface). False: not recorded (no host passes in the runtime,
// OPENREMIX_PROXY_HOST_PASSES=0, a render-target texture sampled, no shader,
// no data): the caller drops it.
bool recordAlways(const proxy::remix::Draw &d, const float projection[16], float depthScale);
// A draw of the engine's depth pass (the DepthWrite shader into a
// render-target texture: addons' depth buffers, _rt_ResolvedFullFrameDepth):
// the pass is replaced, once a frame per target, by one that writes the
// runtime's own view depth as that shader would have (depth / far plane).
// False: no runtime depth or far plane; the caller drops the draw as before.
bool recordDepth(const proxy::remix::Draw &d);
// A StretchRect between the back buffer (nullptr) and render-target textures.
void recordCopy(const void *source, const RECT *sourceRect, const void *target, const RECT *targetRect, bool linear);
// A Clear of a render-target texture's colour.
void recordClear(const void *target, uint32_t argb);
// Hands the frame's passes to the runtime, before Present; `dump` (the flat
// draw dump, when active) receives a description of each.
void submit(FILE *dump);
void dumpStatistics(FILE *out);
} // namespace proxy::hostpass
