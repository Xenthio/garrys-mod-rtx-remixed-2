#include "fixed_function.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace proxy::ff {
namespace {

// Row-major 4x4, row vectors (D3DX): out = a * b.
void multiply(const float *a, const float *b, float *out) {
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      float v = 0;
      for (int k = 0; k < 4; ++k)
        v += a[r * 4 + k] * b[k * 4 + c];
      out[r * 4 + c] = v;
    }
}

// A matrix as the vertex shaders' registers hold it: register k is column k.
void toRegisters(const float *m, float *regs) {
  for (int k = 0; k < 4; ++k)
    for (int j = 0; j < 4; ++j)
      regs[k * 4 + j] = m[j * 4 + k];
}

// D3D's (row vectors, p' = p M) as cModel's 3x4 rows (column vectors).
void toRows(const float *m, float *rows) {
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 4; ++c)
      rows[r * 4 + c] = m[c * 4 + r];
}

// The inverse of an affine matrix (row vectors: the last column 0, 0, 0, 1).
// False when its 3x3 part is singular.
bool inverseAffine(const float *m, float *out) {
  const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
  const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
  if (!std::isfinite(det) || std::fabs(det) < 1e-12f)
    return false;
  const float k = 1 / det;
  const float r[9] = {(e * i - f * h) * k, (c * h - b * i) * k, (b * f - c * e) * k,
                      (f * g - d * i) * k, (a * i - c * g) * k, (c * d - a * f) * k,
                      (d * h - e * g) * k, (b * g - a * h) * k, (a * e - b * d) * k};
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col)
      out[row * 4 + col] = r[row * 3 + col];
    out[row * 4 + 3] = 0;
  }
  for (int col = 0; col < 3; ++col)
    out[12 + col] = -(m[12] * r[col] + m[13] * r[3 + col] + m[14] * r[6 + col]);
  out[15] = 1;
  return true;
}

bool alike(const float *a, const float *b, int n) {
  for (int i = 0; i < n; ++i)
    if (std::fabs(a[i] - b[i]) > 1e-4f * std::max(1.0f, std::fabs(a[i])))
      return false;
  return true;
}

// The camera: the view of the most rigid perspective draws so far this frame
// (the 3D skybox's while it is drawn, then the world's; a lone draw with
// another view, such as one at the origin just before the view model, is
// not it). A blended (skinned) draw's VIEW is not the camera: the shader API
// folds the model's placement into it (VIEW = model to world x camera) and
// its world matrices are the bones relative to the model. The view model's
// draws keep their own projection (its field of view) under the same camera.
struct Camera {
  float view[16];
  uint32_t draws;
};
constexpr size_t kCameras = 64;
std::vector<Camera> g_cameras = [] { // this frame's (at most kCameras: reserved, never moved)
  std::vector<Camera> v;
  v.reserve(kCameras);
  return v;
}();
const Camera *g_camera = nullptr;

void noteCamera(const float *view) {
  Camera *found = nullptr;
  for (auto &c : g_cameras)
    if (std::equal(c.view, c.view + 16, view)) {
      found = &c;
      break;
    }
  if (!found) {
    if (g_cameras.size() >= kCameras)
      return;
    g_cameras.push_back({}); // reserved: g_camera stays valid
    found = &g_cameras.back();
    std::copy_n(view, 16, found->view);
    found->draws = 0;
  }
  ++found->draws;
  if (!g_camera || found->draws >= g_camera->draws)
    g_camera = found;
}

bool identity(const float *m) {
  for (int i = 0; i < 16; ++i)
    if (std::fabs(m[i] - (i % 5 == 0 ? 1.0f : 0.0f)) > 1e-6f)
      return false;
  return true;
}

// The program for the particle path (vsprog): oPos = POSITION x c4..c7,
// oD0 = COLOR0 (white without one), oT0.xy = TEXCOORD0 x c90, c91.
const vsprog::Program *program(bool color) {
  static const auto make = [](bool withColor) {
    std::vector<uint32_t> t = {0xfffe0200,
                               0x0200001f, 0x80000000, 0x900f0000,  // dcl_position v0
                               0x0200001f, 0x80000005, 0x900f0002}; // dcl_texcoord0 v2
    if (withColor)
      t.insert(t.end(), {0x0200001f, 0x8000000a, 0x900f0001}); // dcl_color v1
    for (uint32_t k = 0; k < 4; ++k) // dp4 oPos.{x,y,z,w}, v0, c4+k
      t.insert(t.end(), {0x03000009, 0xc0000000 | (1u << (16 + k)), 0x90e40000, 0xa0e40000 | (kRegModelViewProj + k)});
    t.insert(t.end(), {0x02000001, 0xd00f0000, withColor ? 0x90e40001 : 0xa0e40000 | kRegWhite}); // mov oD0
    for (uint32_t k = 0; k < 2; ++k) // dp4 oT0.{x,y}, v2, c90+k
      t.insert(t.end(), {0x03000009, 0xe0000000 | (1u << (16 + k)), 0x90e40002, 0xa0e40000 | (kRegTexture + k)});
    t.push_back(0x0000ffff);
    return vsprog::compile(t.data(), t.size());
  };
  static const std::shared_ptr<const vsprog::Program> programs[2] = {make(false), make(true)};
  return programs[color].get();
}

const D3DVERTEXELEMENT9 *find(const D3DVERTEXELEMENT9 *e, BYTE usage, BYTE index) {
  for (; e && e->Stream != 0xff; ++e)
    if (e->Usage == usage && e->UsageIndex == index)
      return e;
  return nullptr;
}

bool argIs(DWORD arg, DWORD what) { return (arg & D3DTA_SELECTMASK) == what; }

// Whether a stage operation reads `what` (D3DTA_TEXTURE, D3DTA_TFACTOR).
bool reads(DWORD op, DWORD arg1, DWORD arg2, DWORD arg0, DWORD what) {
  switch (op) {
  case D3DTOP_DISABLE: return false;
  case D3DTOP_SELECTARG1: return argIs(arg1, what);
  case D3DTOP_SELECTARG2: return argIs(arg2, what);
  case D3DTOP_MULTIPLYADD:
  case D3DTOP_LERP: return argIs(arg0, what) || argIs(arg1, what) || argIs(arg2, what);
  default: return argIs(arg1, what) || argIs(arg2, what);
  }
}

bool stageReads(const DWORD *t, DWORD what) {
  return reads(t[D3DTSS_COLOROP], t[D3DTSS_COLORARG1], t[D3DTSS_COLORARG2], t[D3DTSS_COLORARG0], what) ||
         (t[D3DTSS_ALPHAOP] != D3DTOP_DISABLE &&
          reads(t[D3DTSS_ALPHAOP], t[D3DTSS_ALPHAARG1], t[D3DTSS_ALPHAARG2], t[D3DTSS_ALPHAARG0], what));
}

} // namespace

bool stageReadsTexture(const remix::FixedFunction &f, int stage) {
  for (int s = 0; s <= stage; ++s)
    if (f.stages[s][D3DTSS_COLOROP] == D3DTOP_DISABLE)
      return false; // it and every stage above are off
  return stageReads(f.stages[stage], D3DTA_TEXTURE);
}

void newFrame() {
  g_cameras.clear(); // (keeps its capacity)
  g_camera = nullptr;
}

void convert(const remix::Draw &d, int baseStage, Converted &out) {
  const remix::FixedFunction &f = *d.fixedFunction;
  const DWORD *rs = f.renderStates;
  out.skip = nullptr;
  out.baseStage = baseStage;

  // The layout: POSITIONT read as a position, the base stage's coordinates
  // as TEXCOORD0.
  size_t n = 0;
  for (const D3DVERTEXELEMENT9 *e = d.elements; e->Stream != 0xff && n < MAXD3DDECLLENGTH; ++e)
    out.elements[n++] = *e;
  out.elements[n] = D3DVERTEXELEMENT9{0xff, 0, D3DDECLTYPE_UNUSED, 0, 0, 0}; // D3DDECL_END
  out.elementsChanged = false;
  out.pretransformed = false;
  if (!find(out.elements, D3DDECLUSAGE_POSITION, 0))
    for (size_t i = 0; i < n; ++i)
      if (out.elements[i].Usage == D3DDECLUSAGE_POSITIONT && out.elements[i].UsageIndex == 0) {
        out.elements[i].Usage = D3DDECLUSAGE_POSITION;
        out.elements[i].Type = D3DDECLTYPE_FLOAT3; // x, y, z of x, y, z, 1/w
        out.pretransformed = out.elementsChanged = true;
      }
  const DWORD *stage = f.stages[baseStage];
  const DWORD tci = stage[D3DTSS_TEXCOORDINDEX];
  const BYTE input = BYTE(tci & 0xffff);
  out.texgen = (tci & 0xffff0000u) != 0;
  if (input != 0 && !out.texgen)
    for (size_t i = 0; i < n; ++i)
      if (out.elements[i].Usage == D3DDECLUSAGE_TEXCOORD &&
          (out.elements[i].UsageIndex == 0 || out.elements[i].UsageIndex == input)) {
        out.elements[i].UsageIndex = out.elements[i].UsageIndex == 0 ? input : 0;
        out.elementsChanged = true;
      }
  // Generated from the camera-space position (D3DTSS_TCI_CAMERASPACEPOSITION:
  // projected textures, an iris drawn as a pass of its own): the texture
  // coordinates are that position x the texture transform, as world-space
  // rows (view x transform) the mesh projects its vertices with. Other
  // generated coordinates (camera-space normals and reflection vectors, a
  // sphere map: environment-map passes) are not reproduced.
  out.generated = false;
  if (out.texgen) {
    static const bool keep = [] {
      char v[8] = {};
      return GetEnvironmentVariableA("OPENREMIX_PROXY_FF_TEXGEN", v, sizeof(v)) && v[0] == '1';
    }();
    if ((tci & 0xffff0000u) == D3DTSS_TCI_CAMERASPACEPOSITION) {
      float m[16];
      if ((stage[D3DTSS_TEXTURETRANSFORMFLAGS] & 0xff) >= D3DTTFF_COUNT2)
        multiply(f.view, f.textures[baseStage], m);
      else
        std::copy_n(f.view, 16, m);
      for (int a = 0; a < 2; ++a)
        for (int k = 0; k < 4; ++k)
          out.generatedRows[a * 4 + k] = m[k * 4 + a];
      out.generated = true;
    } else if (!keep) {
      out.skip = "base stage generates its coordinates from normals (environment-map pass)";
    }
  }

  // The texture transform of the base stage. A two-component input is
  // extended as (u, v, 1, 0) by D3D: the third row is the translation.
  const DWORD ttff = stage[D3DTSS_TEXTURETRANSFORMFLAGS];
  const DWORD count = ttff & 0xff;
  out.projected = (ttff & D3DTTFF_PROJECTED) != 0;
  out.uvTransform = false;
  const float *tm = f.textures[baseStage];
  if (count >= D3DTTFF_COUNT2 && !out.generated && !identity(tm)) {
    const D3DVERTEXELEMENT9 *uv = find(out.elements, D3DDECLUSAGE_TEXCOORD, 0);
    const int row = uv && uv->Type == D3DDECLTYPE_FLOAT3 ? 3 : 2; // (u, v, w, 1): the fourth
    const float t[8] = {tm[0], tm[4], 0, tm[row * 4], tm[1], tm[5], 0, tm[row * 4 + 1]};
    std::copy_n(t, 8, out.uv);
    out.uvTransform = true;
  } else {
    const float t[8] = {1, 0, 0, 0, 0, 1, 0, 0};
    std::copy_n(t, 8, out.uv);
  }
  out.textureUsed = stageReadsTexture(f, baseStage);
  out.textureFactor = false;
  for (int s = 0; s < 8 && f.stages[s][D3DTSS_COLOROP] != D3DTOP_DISABLE; ++s)
    out.textureFactor |= stageReads(f.stages[s], D3DTA_TFACTOR);
  out.factor = rs[D3DRS_TEXTUREFACTOR];

  // Blending: D3DVBF_nWEIGHTS has n + 1 matrices, D3DVBF_0WEIGHTS one.
  const DWORD vertexBlend = rs[D3DRS_VERTEXBLEND];
  const bool indexed = rs[D3DRS_INDEXEDVERTEXBLENDENABLE] != 0;
  out.tweening = vertexBlend == D3DVBF_TWEENING;
  out.matrices = vertexBlend >= D3DVBF_1WEIGHTS && vertexBlend <= D3DVBF_3WEIGHTS ? vertexBlend + 1
                 : vertexBlend == D3DVBF_0WEIGHTS                              ? 1
                                                                                : 0;
  out.blend = kBlendNone;
  const D3DVERTEXELEMENT9 *indices = find(out.elements, D3DDECLUSAGE_BLENDINDICES, 0);
  if (out.matrices && indexed && indices)
    out.blend = indices->Type == D3DDECLTYPE_D3DCOLOR ? kBlendIndexedColor : kBlendIndexedUByte4;
  else if (out.matrices > 1 && !indexed)
    out.blend = kBlendNonIndexed;
  out.bonesPerVertex = out.blend == kBlendNone ? 0 : std::min<uint32_t>(out.matrices, 3);
  // (Indexed without BLENDINDICES: D3D reads index 0 for every weight, the
  // world matrix alone: rigid.)

  // The view and its registers.
  if (out.pretransformed) {
    // Screen pixels of the viewport to clip space.
    const float w = d.viewport.Width ? float(d.viewport.Width) : 1, h = d.viewport.Height ? float(d.viewport.Height) : 1;
    const float x0 = float(d.viewport.X), y0 = float(d.viewport.Y);
    const float projection[16] = {2 / w, 0, 0, 0, 0, -2 / h, 0, 0, 0, 0, 1, 0, -1 - 2 * x0 / w, 1 + 2 * y0 / h, 0, 1};
    std::copy_n(projection, 16, out.projection);
    std::fill_n(out.view, 16, 0.0f);
    out.view[0] = out.view[5] = out.view[10] = out.view[15] = 1;
  } else {
    std::copy_n(f.view, 16, out.view);
    std::copy_n(f.projection, 16, out.projection);
  }
  out.flat = out.pretransformed ||
             !(std::fabs(std::fabs(out.projection[11]) - 1.0f) < 1e-3f && std::fabs(out.projection[15]) < 1e-3f);
  // A skinned draw is placed under the camera: its VIEW times the camera's
  // inverse is the model's placement, which goes into every bone.
  float placement[16];
  out.placed = false;
  if (!out.flat && out.blend == kBlendNone) {
    noteCamera(out.view);
  } else if (!out.flat && g_camera && !alike(out.view, g_camera->view, 16)) {
    float inverse[16];
    if (inverseAffine(g_camera->view, inverse)) {
      multiply(out.view, inverse, placement);
      std::copy_n(g_camera->view, 16, out.view);
      out.placed = true;
    }
  }
  float viewProjection[16], mvp[16];
  multiply(out.view, out.projection, viewProjection);
  static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  // Every world matrix set so far can be a bone (D3D indexes up to 255).
  const uint32_t bones = out.blend == kBlendNone ? 1 : std::clamp<uint32_t>(f.worldsSet, 1, kMaxWorldMatrices);
  float placedWorld[16];
  if (out.placed)
    multiply(f.worlds[0], placement, placedWorld);
  const float *world = out.pretransformed ? kIdentity : out.placed ? placedWorld : f.worlds[0];
  multiply(world, viewProjection, mvp);
  float *c = out.constants;
  toRegisters(mvp, c + kRegModelViewProj * 4);
  toRegisters(viewProjection, c + kRegViewProj * 4);
  // Bones: the world matrices (bone 0 is D3DTS_WORLD, the rigid draws' model),
  // the registers' share of them as a shader would hold them.
  out.boneRows = bones;
  for (uint32_t i = 0; i < bones; ++i) {
    float placed[16];
    const float *m = i == 0 ? world : f.worlds[i];
    if (i != 0 && out.placed) {
      multiply(f.worlds[i], placement, placed);
      m = placed;
    }
    toRows(m, out.bones + i * 12);
  }
  for (uint32_t i = 0; i < std::min(bones, kMaxBones); ++i)
    std::copy_n(out.bones + i * 12, 12, c + (kRegModel + 3 * i) * 4);
  // Stage 1's camera-space projection (Eyes_dx6's iris): world -> camera ->
  // texture, its first two columns.
  out.iris = false;
  const DWORD *iris = f.stages[1];
  if ((iris[D3DTSS_TEXCOORDINDEX] & 0xffff0000u) == D3DTSS_TCI_CAMERASPACEPOSITION &&
      (iris[D3DTSS_TEXTURETRANSFORMFLAGS] & 0xff) >= D3DTTFF_COUNT2 && f.stages[0][D3DTSS_COLOROP] != D3DTOP_DISABLE &&
      iris[D3DTSS_COLOROP] != D3DTOP_DISABLE) {
    float m[16];
    multiply(f.view, f.textures[1], m);
    for (int a = 0; a < 2; ++a)
      for (int k = 0; k < 4; ++k)
        c[(kRegIrisU + a) * 4 + k] = m[k * 4 + a];
    out.iris = true;
  } else {
    std::fill_n(c + kRegIrisU * 4, 8, 0.0f);
  }
  // The particle program's texture rows (its TEXCOORD0 reads (u, v, 0, 1):
  // the translation goes to w) and white.
  const float rows[8] = {out.uv[0], out.uv[1], 0, out.uv[3], out.uv[4], out.uv[5], 0, out.uv[7]};
  std::copy_n(rows, 8, c + kRegTexture * 4);
  std::fill_n(c + kRegWhite * 4, 4, 1.0f);
  out.program = program(find(out.elements, D3DDECLUSAGE_COLOR, 0) != nullptr);
}

} // namespace proxy::ff
