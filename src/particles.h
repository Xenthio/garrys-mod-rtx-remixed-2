#pragma once

#include "vertex_program.h"

#include <cstdint>

// Particle geometry as the game's vertex shader produces it (vsprog), turned
// into what the Remix raster overlays take: world-space positions, one
// texture coordinate pair and an 8-bit colour per vertex.
//
// Standalone (no Windows headers), like vertex_program: built natively for
// offline checks.
namespace proxy::particles {

struct Vertex {
  float position[3]; // world space
  float uv[2];
  uint32_t color; // D3DCOLOR (memory bytes B, G, R, A), sRGB-encoded rgb
};

struct Request {
  const vsprog::Program *program = nullptr;
  const float *constants = nullptr; // 256 float4 vertex shader registers
  const vsprog::Element *elements = nullptr;
  const vsprog::Stream *streams = nullptr; // 4
  uint32_t first = 0, count = 0;
  // Spritecard: TEXCOORD0 and TEXCOORD1 hold two frames of the sprite sheet
  // and TEXCOORD2.x the second one's weight (the pixel shader blends them):
  // each vertex takes the frame that leads.
  bool frameBlend = false;
  // The pixel shader's constant factors on the colour ($overbrightfactor, a
  // sprite's render colour) and alpha.
  float colorScale[3] = {1, 1, 1};
  float alphaScale = 1;
  // Spritecard $addself: rgb *= 1 + addSelf * alpha (see remix_output).
  float addSelf = 0;
  // Whether the pixel shader takes the vertex colour's rgb / alpha at all
  // (UnlitGeneric: only with $vertexcolor / $vertexalpha).
  bool useColor = true, useAlpha = true;
};

struct Result {
  // The colours are stored divided by this (1, 2 or 4): brighter than white
  // (overbright), shown with a modulate-2x / 4x texture stage.
  uint32_t gain = 1;
  bool anyColor = false, anyAlpha = false; // some vertex has rgb / alpha above zero
  uint32_t secondFrames = 0;               // vertices that took the second sheet frame
  float low[3] = {}, high[3] = {};         // bounds
};

// Runs the draw's vertex shader over vertices first .. first + count - 1 and
// writes them to `out`. The shader's clip-space positions go back to world
// space through the view-projection in c8..c11 (the registers Source's
// shaders take it from; x, y and w suffice for a point, and leave the
// ill-conditioned depth row out). False, with `failure` naming the reason,
// when the shader cannot be run, a vertex is outside its stream, the
// projection is not invertible or a position is not finite.
bool expand(const Request &request, Vertex *out, Result &result, const char **failure);

} // namespace proxy::particles
