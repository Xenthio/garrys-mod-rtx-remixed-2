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
