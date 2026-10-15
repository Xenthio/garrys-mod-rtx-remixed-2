#version 450
// The frame's depth buffer (REMIXAPI_HOST_TARGET_FRAME_DEPTH_EXT) from the
// runtime's view depth, as the game's projection would have written it:
// device z = (p22 * viewZ + p32) / viewZ; 1 (the far plane) where nothing was hit.
// Draws on the frame that test depth (a sky dome behind the scene) see what
// their D3D depth test would have.
layout(binding = 0) uniform sampler2D viewDepth;
layout(push_constant) uniform Push {
  float p22, p32; // the projection's third column, rows 3 and 4 (D3D row vectors)
} push;
layout(location = 0) in vec2 uv;
void main() {
  const float depth = texture(viewDepth, uv).r;
  gl_FragDepth = depth < 1e9 ? clamp((push.p22 * depth + push.p32) / depth, 0, 1) : 1;
}
