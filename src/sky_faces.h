#pragma once
#include <cmath>

// World-aligned cube faces for capturing the 2D skybox (Source: Z up). Face
// f looks along `forward` with `up`; `right` = forward x up. Shared by the
// capture (D3D9) and the equirectangular resampling (Remix dome).
namespace proxy::sky {

constexpr unsigned kFaceSize = 256;

inline void faceBasis(int face, float right[3], float up[3], float forward[3]) {
  static const float kForward[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  static const float kUp[6][3] = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}};
  const float *d = kForward[face], *u = kUp[face];
  right[0] = d[1] * u[2] - d[2] * u[1];
  right[1] = d[2] * u[0] - d[0] * u[2];
  right[2] = d[0] * u[1] - d[1] * u[0];
  // up' = right x forward (orthonormal; equals u for these faces)
  up[0] = right[1] * d[2] - right[2] * d[1];
  up[1] = right[2] * d[0] - right[0] * d[2];
  up[2] = right[0] * d[1] - right[1] * d[0];
  for (int i = 0; i < 3; ++i)
    forward[i] = d[i];
}

} // namespace proxy::sky
