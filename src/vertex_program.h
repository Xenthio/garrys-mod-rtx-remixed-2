#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// Direct3D 9 vertex shaders (vs_1_1 .. vs_3_0 token streams) run on the CPU.
//
// Source's particle shaders (Spritecard, SplineCard) build their geometry in
// the vertex shader: every vertex of a particle carries the particle's centre
// as POSITION, and its corner, size, rotation, orientation and sheet frames
// come from other vertex elements and shader constants. D3D rasterizes nothing
// here, so the geometry the vertex shader would have produced is computed by
// running the game's own shader, whatever it is (five orientation modes, size
// clamps and distance fades per build, addon shaders), instead of a copy of
// one build's shader written by hand.
//
// Standalone (no Windows headers): also built natively for offline checks
// against an independent evaluator.
namespace proxy::vsprog {

// Vertex shader outputs (vs_3_0 outputs map here by their declared usage).
enum Slot : unsigned {
  kPosition, // clip space
  kFog,
  kPointSize,
  kColor0,
  kColor1,
  kTex0,
  kTex7 = kTex0 + 7,
  kSlots
};

// D3DVERTEXELEMENT9's layout; a declaration ends with stream 0xff.
struct Element {
  uint16_t stream, offset;
  uint8_t type, method, usage, usageIndex;
};

// One vertex stream: `data` is its vertex 0 (the stream offset and the draw's
// base vertex applied by the caller), `size` the bytes readable from there.
struct Stream {
  const uint8_t *data = nullptr;
  size_t size = 0;
  uint32_t stride = 0;
};

struct Input {
  uint8_t reg, usage, usageIndex; // D3DDECLUSAGE
};

struct Info {
  uint64_t hash = 0;      // of the token stream
  uint32_t tokens = 0;    // its length, END included
  uint16_t version = 0;   // 0x0200: vs_2_0
  uint32_t instructions = 0;
  bool supported = false; // can be run
  const char *reason = ""; // why not
  // The clip-space position depends on texture coordinates or colours: the
  // shader builds geometry (billboards, ribbons) from per-vertex parameters.
  // (Normals, tangents and further POSITION elements do not count: Source's
  // model shaders add a flex delta, POSITION1, to every position.)
  bool expands = false;
  // It depends on bone weights or indices (skinning).
  bool skinned = false;
  uint32_t written = 0; // output slots written (1 << Slot)
  // The output whose rgb carries the vertex colour for the pixel shader (-1:
  // none): the first texture coordinate computed from the COLOR input alone
  // (Source's generic and Sprite shaders pass it in TEXCOORD2; their COLOR0
  // is lighting), else COLOR0 when it depends on the COLOR input (Spritecard:
  // colour x distance fades) or on constants alone (a modulation colour).
  // `alphaSlot`: where its alpha is (that slot's w when written, else
  // COLOR0's; -1: opaque). `colorLinear`: the rgb went through the shader's
  // gamma-to-linear conversion (pow 2.2); informational.
  int colorSlot = -1, alphaSlot = -1;
  bool colorLinear = false;
  std::vector<Input> inputs;
  std::vector<uint16_t> constants; // float registers read (those the shader does not define)
};

struct Program;

// Decodes a token stream (as given to CreateVertexShader). nullptr when it is
// not a vertex shader or runs past `maxTokens` without an END.
std::shared_ptr<const Program> compile(const uint32_t *tokens, size_t maxTokens = 1 << 16);
const Info &info(const Program &program);
const std::vector<uint32_t> &tokens(const Program &program);

// Runs vertices first .. first + count - 1 of a declaration through the
// program. `constants`: 256 float4 registers. `out`: for each slot set in
// `slots`, in ascending slot order, `count` float4 values (an unwritten
// component is 0, w 1). False when the program is not supported or a vertex
// lies outside its stream.
bool run(const Program &program, const float *constants, const Element *elements, const Stream streams[4],
         uint32_t first, uint32_t count, uint32_t slots, float *out);

} // namespace proxy::vsprog
