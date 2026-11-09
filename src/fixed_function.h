#pragma once
#include "remix_output.h"
#include "vertex_program.h"

#include <cstdint>
#include <memory>

// Draws made through D3D9's fixed-function pipeline (no vertex shader bound:
// Half-Life 2 RTX, whose engine build NVIDIA made for RTX Remix, draws
// everything so), restated in the terms of the shader draws the rest of the
// proxy reads: the vertex shader registers Source's shaders take their
// matrices from, filled from D3D's transforms, a layout whose TEXCOORD0 is the
// base texture's coordinates, the bone matrices of D3D's vertex blending, and
// a vertex program that does what the fixed-function transform does (for the
// particle path, which runs the draw's program on the CPU).
//
// What carries over:
//   camera     D3DTS_VIEW and D3DTS_PROJECTION are the view's matrices (the
//              shader draws read them through IShaderAPI::GetMatrix); their
//              product, in c8..c11, names the view, as cViewProj does;
//   rigid      D3DTS_WORLD is the model matrix (c58..c60, cModel[0]) and
//              world x view x projection the c4..c7 cModelViewProj;
//   blended    D3DRS_VERTEXBLEND (n weights: n + 1 matrices; D3DVBF_0WEIGHTS:
//              one) and D3DRS_INDEXEDVERTEXBLENDENABLE: the matrices
//              D3DTS_WORLDMATRIX(i) are bones i (c58 + 3 i, as cModel[i]),
//              indexed by BLENDINDICES (D3DCOLOR read as D3D does, the
//              components R, G, B, A in memory bytes 2, 1, 0, 3; UBYTE4 in
//              order) or, without indexing, matrix k for weight k; the last
//              weight is 1 minus the stored ones, as in Source's skinning;
//   2D         a projection without perspective is a flat draw (the UI path,
//              which takes c4..c7); pre-transformed vertices (POSITIONT,
//              D3DFVF_XYZRHW) are screen pixels: an orthographic projection
//              of the viewport stands in;
//   texture    the base texture is stage 0's, unless the material's
//              $basetexture is bound on another stage (a stage 0 lightmap):
//              its TEXCOORDINDEX picks the coordinates, its texture transform
//              (D3DTTFF_COUNT2.., D3DTS_TEXTUREn, Source's scrolling and
//              $basetexturetransform) is applied to them; a stage whose
//              colour and alpha do not read the texture (D3DTOP_DISABLE,
//              SELECTARG of DIFFUSE) leaves the draw untextured;
//   eyes       Eyes_dx6 projects the iris on stage 1 from the camera-space
//              position (D3DTSS_TCI_CAMERASPACEPOSITION and a texture
//              transform): as world-space rows that is view x transform,
//              which goes to c50/c51 where the Eyes shader keeps its
//              projection.
// Base-stage coordinates generated from the camera-space position (projected
// textures, an iris as a pass of its own) are projected from the world
// position likewise (`generatedRows`). Those generated from normals
// (camera-space normals, reflection vectors, sphere maps: environment-map
// passes, which the path tracer's reflections replace) are dropped
// (OPENREMIX_PROXY_FF_TEXGEN=1 keeps them, with the vertex coordinates).
namespace proxy::ff {

// The registers (as remix_output's: Source's common_vs_fxc.h layout).
constexpr unsigned kRegModelViewProj = 4, kRegViewProj = 8, kRegIrisU = 50, kRegModel = 58, kMaxBones = 53;
// D3D's world matrices: WORLDMATRIX(0..255), more than the registers hold.
constexpr unsigned kMaxWorldMatrices = 256;
// The synthesized program's texture transform rows and its white: above the
// bones (c58 .. c58 + 3 x 53 - 1 = c216), which every draw's registers carry.
constexpr unsigned kRegTexture = 240, kRegWhite = 242;
static_assert(kRegTexture >= kRegModel + 3 * kMaxBones, "the particle program's registers overlap the bones");

// How a draw's vertices pick their bones (buildMesh).
enum Blend : uint8_t {
  kBlendNone,         // rigid (D3DTS_WORLD), or a shader draw
  kBlendIndexedColor, // indexed, D3DCOLOR indices (swizzled)
  kBlendIndexedUByte4,
  kBlendNonIndexed,   // weight k on matrix k
};

struct Converted {
  float constants[256 * 4] = {}; // only the registers named above are written
  float bones[kMaxWorldMatrices * 12] = {}; // the bones as cModel's rows (the first kMaxBones also in `constants`)
  uint32_t boneRows = 0;          // how many of `bones` are written
  float view[16], projection[16]; // the view's (GetMatrix's form: D3D's)
  D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH + 1];
  bool elementsChanged = false;   // `elements` differs from the draw's
  bool pretransformed = false;    // POSITIONT vertices
  bool flat = false;              // no perspective (pre-transformed included)
  Blend blend = kBlendNone;
  uint32_t matrices = 0;          // D3D's matrices per vertex (0: blending off)
  uint32_t bonesPerVertex = 0;    // as the mesh takes them (at most 3)
  bool tweening = false;          // D3DVBF_TWEENING (drawn rigid)
  bool placed = false;            // a blended draw's VIEW held its model's placement: moved into the bones
  int baseStage = 0;              // the stage whose texture is the base texture
  bool textureUsed = true;        // that stage reads its texture
  bool uvTransform = false;       // `uv` applies (u' = uv0 u + uv1 v + uv3, v' = uv4 u + uv5 v + uv7)
  float uv[8] = {1, 0, 0, 0, 0, 1, 0, 0};
  bool projected = false;         // D3DTTFF_PROJECTED on the base stage (not divided)
  bool texgen = false;            // the base stage generates its coordinates
  bool generated = false;         // ... from the camera-space position: `generatedRows` give them
  float generatedRows[8] = {};    // u = row 0 . (world, 1), v = row 1 . (world, 1)
  bool iris = false;              // c50/c51 hold stage 1's camera-space projection
  bool textureFactor = false;     // a used stage takes D3DTA_TFACTOR
  uint32_t factor = 0xffffffff;   // D3DRS_TEXTUREFACTOR (D3DCOLOR)
  const vsprog::Program *program = nullptr;
  const char *skip = nullptr;     // why the draw is dropped (nullptr: it is drawn)
};

// Fills `out` from the draw's fixed-function state. `baseStage`: the stage
// holding the base texture (0 unless the caller found it elsewhere).
void convert(const remix::Draw &d, int baseStage, Converted &out);

// A new frame: the cameras convert() has seen are forgotten (render thread).
void newFrame();

// Whether stage `stage` (with every stage below it enabled) reads its texture.
bool stageReadsTexture(const remix::FixedFunction &f, int stage);

} // namespace proxy::ff
