#pragma once

namespace proxy::source_rendering {
// Keep the standalone BSP host and live adapter in the same Source-unit
// convention. The caller chooses defaults versus explicit settings, preserving
// its own profile/override policy. Native renderers remain engine agnostic.
template <typename Setter> void lightingDefaults(Setter set) {
  // A 16-world-unit local reach, appropriate to metre-scale scenes, would
  // extinguish Source lamps beyond roughly 40 cm. Authored lights retain their
  // physical falloff; zero removes only this additional runtime cutoff.
  set("rtx.native.pathtrace.localRange", "0");
  // Source intensities otherwise saturate the metre-tuned adaptive ray budget.
  set("rtx.native.pathtrace.adaptiveLocalSampling", "0");
  set("rtx.native.pathtrace.sceneScale", "0.0254");
  set("rtx.native.forward.sceneScale", "0.0254");
}
} // namespace proxy::source_rendering
