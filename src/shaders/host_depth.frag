#version 450
// The runtime's view depth (REMIXAPI_HOST_TARGET_VIEW_DEPTH_EXT: the distance
// along the camera's axis, game units, 1e10 and above where nothing was hit)
// written as the engine's DepthWrite shader writes a depth pass: depth / far
// plane in red, the target's clear value where there is no surface.
layout(binding = 0) uniform sampler2D viewDepth;
layout(push_constant) uniform Push {
  float scale; // 1 / far plane
  float none;  // the clear value's red
} push;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
void main() {
  const float depth = texture(viewDepth, uv).r;
  color = vec4(depth < 1e9 ? depth * push.scale : push.none, 0, 0, 1);
}
