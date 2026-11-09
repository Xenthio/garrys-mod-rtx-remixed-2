// Left 4 Dead 2: the common infected (Source's Infected shader).
//
// One texture set is many infected: the pixel shader maps the base texture's
// luminance (alpha) through a 256x16 gradient, a row for skin and a row for
// cloth picked per infected (g_vUVTransform.zw), masked by the base's blue;
// a quarter-size sprite of the base (offset g_vUVTransform.xy) holds grime
// (red: the detail texture) and blood (green: g_cBloodColor, ranged by
// g_vGradSelect.xy). Wounds: the vertex shader maps the bind-pose position
// into two ellipsoids (centre, side, up, look-at registers) and projects it
// into the cut texture (g_vWoundUvScales / Biases); the pixel shader discards
// where the cut texture (scale-biased by g_vWoundUVScaleBias1/2) is above
// g_vGradSelect.z, and deepens the blood there. Burning (BurnSampler): the
// burn texture, read at the detail coordinates swapped, takes over as
// g_vShaderParams.x rises, skin last. Fading: the eye-glow and Phong
// combinations write g_DiffuseModulation's alpha as the pixel's, and an
// alpha-blended draw of theirs is a model fading out.
//
// Here the draws are told apart by their shaders' constant tables, and their
// constants become remixapi_MaterialGradientMapEXT (remix_output: the
// material, cached) and remixapi_InstanceInfoWoundCutEXT (the instance).
#include "l4d2.h"

#include "bsp_world.h"
#include "d3d_hooks.h"
#include "shader_tables.h"

#include <d3d9.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace proxy::l4d2::infected {
namespace {

using shader_tables::Cache;
using shader_tables::ConstantTable;
using shader_tables::lookup;

// What a pixel shader is (registers -1: not declared).
struct PixelShader {
  bool infected = false, wounded = false;
  int gradSelect = -1, blood = -1, wound1 = -1, wound2 = -1, uvTransform = -1;
  int eyeGlow = -1, modulation = -1, shaderParams = -1;
  int base = -1, detail = -1, gradient = -1, cut = -1, burn = -1;
};
struct VertexShader {
  bool known = false, wound = false;
  int detail = -1, scales = -1, biases = -1;
  int center = -1, lookAt = -1, up = -1, side = -1, center2 = -1, lookAt2 = -1, up2 = -1, side2 = -1;
};

Cache<PixelShader> g_pixelShaders;
Cache<VertexShader> g_vertexShaders;

PixelShader describePixel(const std::vector<uint32_t> &code) {
  PixelShader s;
  ConstantTable t;
  if (!shader_tables::parse(code, t))
    return s;
  s.gradSelect = t.floatRegister("g_vGradSelect");
  s.blood = t.floatRegister("g_cBloodColor_WaterFogOORange");
  s.uvTransform = t.floatRegister("g_vUVTransform");
  s.wound1 = t.floatRegister("g_vWoundUVScaleBias1");
  s.wound2 = t.floatRegister("g_vWoundUVScaleBias2");
  s.base = t.sampler("BaseTextureSampler");
  s.detail = t.sampler("DetailSampler");
  s.gradient = t.sampler("ColorLookupSampler");
  s.cut = t.sampler("g_tCutOutDecalMap");
  s.burn = t.sampler("BurnSampler");
  s.eyeGlow = t.floatRegister("g_cEyeGlowColor");
  s.modulation = t.floatRegister("g_DiffuseModulation");
  s.shaderParams = t.floatRegister("g_vShaderParams");
  s.infected = s.gradSelect >= 0 && s.blood >= 0 && s.uvTransform >= 0 && s.base >= 0 && s.gradient >= 0 &&
               s.gradSelect < 224 && s.blood < 224 && s.uvTransform < 224 && s.base < 16 && s.gradient < 16;
  s.wounded = s.infected && s.cut >= 0 && s.cut < 16 && s.wound1 >= 0 && s.wound1 < 224 && s.wound2 >= 0 &&
              s.wound2 < 224;
  if (s.detail >= 16)
    s.detail = -1;
  for (int *r : {&s.eyeGlow, &s.modulation, &s.shaderParams})
    if (*r >= 224)
      *r = -1;
  if (s.burn >= 16 || s.shaderParams < 0)
    s.burn = -1;
  return s;
}

VertexShader describeVertex(const std::vector<uint32_t> &code) {
  VertexShader s;
  ConstantTable t;
  if (!shader_tables::parse(code, t))
    return s;
  s.detail = t.floatRegister("cDetailTexCoordTransform");
  s.scales = t.floatRegister("g_vWoundUvScales");
  s.biases = t.floatRegister("g_vWoundUvBiases");
  s.center = t.floatRegister("g_vEllipsoidCenter");
  s.lookAt = t.floatRegister("g_vEllipsoidLookAt");
  s.up = t.floatRegister("g_vEllipsoidUp");
  s.side = t.floatRegister("g_vEllipsoidSide");
  s.center2 = t.floatRegister("g_vEllipsoidCenter2");
  s.lookAt2 = t.floatRegister("g_vEllipsoidLookAt2");
  s.up2 = t.floatRegister("g_vEllipsoidUp2");
  s.side2 = t.floatRegister("g_vEllipsoidSide2");
  s.known = s.detail >= 0 && s.detail + 1 < 256;
  s.wound = true;
  for (int r : {s.scales, s.biases, s.center, s.lookAt, s.up, s.side, s.center2, s.lookAt2, s.up2, s.side2})
    s.wound = s.wound && r >= 0 && r < 256;
  return s;
}

bool finite(const float *v, int n) {
  for (int i = 0; i < n; ++i)
    if (!std::isfinite(v[i]))
      return false;
  return true;
}

// Unused ellipsoids sit far away (the game's 1e6): not near any model.
bool nearby(const float *center) {
  return finite(center, 3) && std::fabs(center[0]) < 1e5f && std::fabs(center[1]) < 1e5f &&
         std::fabs(center[2]) < 1e5f;
}

// 0 to 1 in steps of 1 / Params::kQuantum (not finite: 0).
float quantize(float v) {
  return std::isfinite(v) ? std::round(std::clamp(v, 0.0f, 1.0f) * Params::kQuantum) / Params::kQuantum : 0;
}

struct Stats {
  std::atomic<uint64_t> draws{0}, wounded{0}, woundSent{0}, materials{0}, materialsCreated{0}, materialsFailed{0},
      woundCombo{0}, noVertexTable{0}, burning{0}, faded{0};
  std::atomic<uint64_t> refused{0}, refusedBlend{0}, refusedFadeable{0}, refusedAlphaTest{0}, refusedLifted{0},
      refusedParticle{0}, refusedWoundCombo{0}, refusedWounded{0};
  std::atomic<uint64_t> missing[kMissingCount] = {};
} g_stats;

// The probe (render thread).
uint32_t g_probeLeft = 0;
// "wounded [count]": only draws with an ellipsoid near the model; "refused
// [count]": only draws not taken (the others only those taken).
enum ProbeMode { kProbeTaken, kProbeWounded, kProbeRefused } g_probeMode = kProbeTaken;
uint64_t g_probeSkipped = 0;
std::wstring g_probePath;
std::vector<const void *> g_probeNamed; // pixel shaders whose names this probe printed

} // namespace

bool enabled() {
  static const bool on = envFlag("INFECTED", true);
  return on;
}

bool classify(const void *pixelShader, const void *vertexShader, const float *ps, const float *vs, bool alphaBlended,
              Params &out) {
  if (!ps || !vs)
    return false;
  const PixelShader *p = lookup(g_pixelShaders, pixelShader, describePixel);
  if (!p || !p->infected)
    return false;
  const VertexShader *v = lookup(g_vertexShaders, vertexShader, describeVertex);
  out = Params{};
  out.baseStage = p->base;
  out.gradientStage = p->gradient;
  out.detailStage = p->detail;
  out.eyeGlowRegister = p->eyeGlow;
  out.modulationRegister = p->modulation;
  out.shaderParamsRegister = p->shaderParams;
  out.fadeable = p->modulation >= 0;
  out.burnStage = p->burn;
  if (p->burn >= 0)
    out.burnStrength = quantize(ps[p->shaderParams * 4]);
  if (alphaBlended && p->modulation >= 0)
    out.opacity = quantize(ps[p->modulation * 4 + 3]);
  const float *gradSelect = ps + p->gradSelect * 4, *blood = ps + p->blood * 4, *uv = ps + p->uvTransform * 4;
  out.spriteOffset[0] = uv[0];
  out.spriteOffset[1] = uv[1];
  out.skinRow = uv[2];
  out.clothRow = uv[3];
  std::copy_n(blood, 3, out.bloodColor);
  std::copy_n(gradSelect, 2, out.bloodRange);
  // The pixel shader reads the detail texture at (dot(c[d + 1].xy, uv),
  // dot(c[d].xy, uv)): u from the transform's second row, v from its first.
  if (v && v->known) {
    out.vertexShaderKnown = true;
    std::copy_n(vs + (v->detail + 1) * 4, 2, out.detailU);
    std::copy_n(vs + v->detail * 4, 2, out.detailV);
  } else
    ++g_stats.noVertexTable;
  if (!finite(out.spriteOffset, 2) || !std::isfinite(out.skinRow) || !std::isfinite(out.clothRow) ||
      !finite(out.bloodColor, 3) || !finite(out.bloodRange, 2) || !finite(out.detailU, 2) || !finite(out.detailV, 2))
    return false;
  out.woundCombo = p->wounded;
  if (p->wounded) {
    ++g_stats.woundCombo;
    out.cutStage = p->cut;
    std::copy_n(ps + p->wound1 * 4, 4, out.cutScaleBias[0]);
    std::copy_n(ps + p->wound2 * 4, 4, out.cutScaleBias[1]);
    out.threshold = gradSelect[2];
  }
  if (p->wounded && v && v->wound) {
    // e = (dot(side, p - c), -dot(up, p - c), dot(lookAt, p - c)).
    const int centers[2] = {v->center, v->center2}, sides[2] = {v->side, v->side2}, ups[2] = {v->up, v->up2},
              looks[2] = {v->lookAt, v->lookAt2};
    for (int e = 0; e < 2; ++e)
      for (int c = 0; c < 3; ++c) {
        out.center[e][c] = vs[centers[e] * 4 + c];
        out.axisX[e][c] = vs[sides[e] * 4 + c];
        out.axisY[e][c] = -vs[ups[e] * 4 + c];
        out.axisZ[e][c] = vs[looks[e] * 4 + c];
      }
    out.mappingScale = vs[v->lookAt * 4 + 3];
    out.secondBlend = vs[v->side2 * 4 + 3];
    std::copy_n(vs + v->scales * 4, 4, out.projectionScale);
    std::copy_n(vs + v->biases * 4, 4, out.projectionBias);
    // A far ellipsoid's axes still give finite coordinates: only centres may be huge.
    out.wounded = (nearby(out.center[0]) || nearby(out.center[1])) && std::isfinite(out.mappingScale) &&
                  out.mappingScale != 0 && std::isfinite(out.secondBlend) && finite(out.projectionScale, 4) &&
                  finite(out.projectionBias, 4) && finite(out.cutScaleBias[0], 8) && std::isfinite(out.threshold) &&
                  finite(&out.axisX[0][0], 6) && finite(&out.axisY[0][0], 6) && finite(&out.axisZ[0][0], 6);
  }
  return true;
}

void noteDraw(const Params &p, bool material, bool woundSent) {
  ++g_stats.draws;
  g_stats.wounded += p.wounded;
  g_stats.materials += material;
  g_stats.woundSent += woundSent;
  g_stats.burning += p.burnStrength > 0;
  g_stats.faded += p.opacity < 1;
}
void noteRefused(bool blend, bool alphaTest, bool lifted, bool particle, const Params &p) {
  ++g_stats.refused;
  g_stats.refusedBlend += blend;
  g_stats.refusedFadeable += blend && p.fadeable;
  g_stats.refusedAlphaTest += alphaTest;
  g_stats.refusedLifted += lifted;
  g_stats.refusedParticle += particle;
  g_stats.refusedWoundCombo += p.woundCombo;
  g_stats.refusedWounded += p.woundCombo && (nearby(p.center[0]) || nearby(p.center[1]));
}
void noteMissing(Missing which) { ++g_stats.missing[which]; }
void noteMaterialCreated(bool failed) { ++(failed ? g_stats.materialsFailed : g_stats.materialsCreated); }

void dumpStatistics(FILE *out, size_t materials) {
  static ULONGLONG last = GetTickCount64();
  const ULONGLONG now = GetTickCount64();
  const double seconds = now > last ? double(now - last) / 1000 : 1;
  last = now;
  auto rate = [&](std::atomic<uint64_t> &counter) { return double(counter.exchange(0)) / seconds; };
  if (!enabled()) {
    fprintf(out, "== L4D2 infected: off (OPENREMIX_PROXY_INFECTED=0)\n");
    return;
  }
  fprintf(out,
          "== L4D2 infected: per second: draws %.1f (gradient material %.1f; wounded combination %.1f, wounded %.1f, "
          "wound cut sent %.1f; burning %.1f, faded %.1f; vertex shader without a constant table %.1f) | materials "
          "made %.1f, failed %.1f, cached %zu | textures missing: base %.1f gradient %.1f detail %.1f cut %.1f burn "
          "%.1f | shaders known: %zu pixel, %zu vertex\n",
          rate(g_stats.draws), rate(g_stats.materials), rate(g_stats.woundCombo), rate(g_stats.wounded),
          rate(g_stats.woundSent), rate(g_stats.burning), rate(g_stats.faded), rate(g_stats.noVertexTable),
          rate(g_stats.materialsCreated), rate(g_stats.materialsFailed), materials,
          rate(g_stats.missing[kMissingBase]), rate(g_stats.missing[kMissingGradient]),
          rate(g_stats.missing[kMissingDetail]), rate(g_stats.missing[kMissingCut]),
          rate(g_stats.missing[kMissingBurn]), g_pixelShaders.size(), g_vertexShaders.size());
  fprintf(out,
          "   infected shader draws refused per second: %.1f (blended %.1f, of which declaring g_DiffuseModulation "
          "%.1f; alpha tested %.1f, lifted %.1f, particle %.1f; wounded combination %.1f, with a wound near the "
          "model %.1f)\n",
          rate(g_stats.refused), rate(g_stats.refusedBlend), rate(g_stats.refusedFadeable),
          rate(g_stats.refusedAlphaTest), rate(g_stats.refusedLifted), rate(g_stats.refusedParticle),
          rate(g_stats.refusedWoundCombo), rate(g_stats.refusedWounded));
}

// "shaders": every model shader seen so far, with the names its constant table declares.
template <typename Info, typename Flags>
void dumpShaders(FILE *out, const char *kind, const Cache<Info> &cache, Flags flags) {
  for (const auto &[shader, entry] : cache) {
    fprintf(out, "%s %p, %zu words, %s:", kind, shader, entry.code->size(), flags(entry.info));
    ConstantTable t;
    if (!shader_tables::parse(*entry.code, t))
      fprintf(out, " (no constant table)");
    static const char *sets[4] = {"b", "i", "c", "s"};
    for (const auto &e : t.entries)
      fprintf(out, " %s=%s%u", e.name.c_str(), e.set < 4 ? sets[e.set] : "?", unsigned(e.index));
    fprintf(out, "\n");
  }
}

void servicePresent(const wchar_t *directory, uint64_t frame) {
  if (frame % 16)
    return;
  const std::wstring dir = directory;
  const std::wstring request = dir + L"\\shaderapi_proxy_infected.request";
  if (GetFileAttributesW(request.c_str()) == INVALID_FILE_ATTRIBUTES)
    return;
  int count = 32;
  g_probeMode = kProbeTaken;
  if (FILE *in = _wfopen(request.c_str(), L"r")) {
    char line[64] = {};
    if (std::fgets(line, sizeof(line), in)) {
      const char *number = line;
      if (line[0] == 's' || line[0] == 'S') {
        fclose(in);
        DeleteFileW(request.c_str());
        if (FILE *out = _wfopen((dir + L"\\infected_shaders.txt").c_str(), L"w")) {
          fprintf(out, "== model shaders seen, frame %llu\n", static_cast<unsigned long long>(frame));
          dumpShaders(out, "ps", g_pixelShaders, [](const PixelShader &p) {
            return p.wounded ? "infected, wounded" : p.infected ? "infected" : "other";
          });
          dumpShaders(out, "vs", g_vertexShaders, [](const VertexShader &v) {
            return v.wound ? "detail, wound" : v.known ? "detail" : "other";
          });
          fclose(out);
        }
        return;
      }
      if (line[0] == 'w' || line[0] == 'W' || line[0] == 'r' || line[0] == 'R') {
        g_probeMode = line[0] == 'w' || line[0] == 'W' ? kProbeWounded : kProbeRefused;
        number = std::strpbrk(line, "0123456789");
        number = number ? number : "";
      }
      if (std::atoi(number) > 0)
        count = std::atoi(number);
    }
    fclose(in);
  }
  g_probeSkipped = 0;
  g_probeNamed.clear();
  DeleteFileW(request.c_str());
  g_probeLeft = uint32_t(std::min(count, 4096));
  g_probePath = dir + L"\\infected_probe.txt";
  if (FILE *out = _wfopen(g_probePath.c_str(), L"w")) {
    static const char *modes[3] = {"", "wounded ", "refused (not taken) "};
    fprintf(out, "== infected probe: the next %u %sinfected draws, from frame %llu (OPENREMIX_PROXY_INFECTED %s)\n",
            g_probeLeft, modes[g_probeMode], static_cast<unsigned long long>(frame), enabled() ? "on" : "off");
    fclose(out);
  }
}

bool probing() { return g_probeLeft != 0; }

void probe(const Params &p, const float *ps, const float *vs, const ProbeDraw &draw) {
  if (!g_probeLeft || draw.refused != (g_probeMode == kProbeRefused))
    return;
  if (g_probeMode == kProbeWounded && !(p.woundCombo && (nearby(p.center[0]) || nearby(p.center[1])))) {
    ++g_probeSkipped;
    return;
  }
  --g_probeLeft;
  FILE *out = _wfopen(g_probePath.c_str(), L"a");
  if (!out)
    return;
  if (g_probeMode == kProbeWounded)
    fprintf(out, "(%llu unwounded draws skipped before this one)\n", static_cast<unsigned long long>(g_probeSkipped));
  g_probeSkipped = 0;
  // Textures the shader is known to bind, by the hashes of their files.
  struct Known {
    const char *name;
    std::vector<uint64_t> hashes;
  };
  static std::vector<Known> known = [] {
    std::vector<Known> k;
    for (const char *name : {"models/infected/common/body_wounds/wound_combined_cull_texture",
                             "models/infected/common/body_wounds/wound_cull_texture",
                             "models/infected/common/body_wounds/wound_slash_cull_texture",
                             "models/infected/common/l4d2/ci_gradient_palette",
                             "models/infected/common/l4d2/detail/ci_detail_dirt",
                             "models/infected/common/l4d2/ci_burning"})
      k.push_back({name, bsp::textureCandidates(name)});
    return k;
  }();
  if (draw.refused)
    fprintf(out, "f%llu REFUSED (blended %d, alpha tested %d, lifted %d, particle %d) verts %u prims %u | wounded "
                 "combination %d\n",
            static_cast<unsigned long long>(draw.frame), int(draw.blend), int(draw.alphaTest), int(draw.lifted),
            int(draw.particle), draw.vertices, draw.primitives, int(p.woundCombo));
  else
    fprintf(out, "f%llu verts %u prims %u | layout skinned %d, bones/vertex %u, bones %u -> %s | dynamic vertices "
                 "%d%s | material %s | wounded %d (combination %d), cut sent %d\n",
            static_cast<unsigned long long>(draw.frame), draw.vertices, draw.primitives, int(draw.skinnedLayout),
            draw.bonesPerVertex, draw.boneCount,
            draw.boneCount > 1    ? "bind pose + bone transforms"
            : draw.boneCount == 1 ? "bind pose, rigid (model transform)"
            : draw.skinnedLayout  ? "CPU-skinned vertices (world space)"
                                  : "unskinned layout (model transform)",
            int(draw.dynamicVertices), draw.flexIgnored ? " | flex stream ignored" : "",
            draw.material ? "gradient map" : "plain (fallback)", int(p.wounded), int(p.woundCombo),
            int(draw.woundSent));
  // D3DBLEND / D3DBLENDOP / D3DCMP values as numbers (SRCALPHA 5, INVSRCALPHA 6, ONE 2; ADD 1).
  fprintf(out, "   blend %d src %u dst %u op %u | separate alpha %d src %u dst %u op %u | alpha test %d func %u ref %u\n",
          int(draw.blend), draw.srcBlend, draw.dstBlend, draw.blendOp, int(draw.separateAlpha), draw.srcBlendAlpha,
          draw.dstBlendAlpha, draw.blendOpAlpha, int(draw.alphaTest), draw.alphaFunc, draw.alphaRef);
  // The pixel shader's names, the first time this probe meets it.
  if (draw.pixelShader &&
      std::find(g_probeNamed.begin(), g_probeNamed.end(), draw.pixelShader) == g_probeNamed.end()) {
    g_probeNamed.push_back(draw.pixelShader);
    auto it = g_pixelShaders.find(draw.pixelShader);
    ConstantTable t;
    if (it != g_pixelShaders.end() && shader_tables::parse(*it->second.code, t)) {
      fprintf(out, "   ps %p declares:", draw.pixelShader);
      static const char *sets[4] = {"b", "i", "c", "s"};
      for (const auto &e : t.entries)
        fprintf(out, " %s=%s%u", e.name.c_str(), e.set < 4 ? sets[e.set] : "?", unsigned(e.index));
      fprintf(out, "\n");
    }
  }
  auto reg = [&](const char *label, const float *bank, int r) {
    const float *c = bank + r * 4;
    fprintf(out, " %s%d (%g %g %g %g)", label, r, c[0], c[1], c[2], c[3]);
  };
  fprintf(out, "   ps:");
  for (int r : {3, 4, 7, 8, 10})
    reg("c", ps, r);
  fprintf(out, "\n   ps:");
  const struct {
    const char *name;
    int r;
  } named[3] = {{"eye glow", p.eyeGlowRegister}, {"modulation", p.modulationRegister},
                {"shader params", p.shaderParamsRegister}};
  for (const auto &n : named) {
    fprintf(out, " %s", n.name);
    if (n.r >= 0)
      reg("c", ps, n.r);
    else
      fprintf(out, " -");
  }
  fprintf(out, "\n   vs:");
  for (int r : {14, 15, 48, 49, 50, 51})
    reg("c", vs, r);
  fprintf(out, "\n   vs:");
  for (int r = 52; r <= 57; ++r)
    reg("c", vs, r);
  fprintf(out, "\n   taken: rows skin %g cloth %g sprite %g %g detail u (%g %g) v (%g %g) blood (%g %g %g) range %g..%g",
          p.skinRow, p.clothRow, p.spriteOffset[0], p.spriteOffset[1], p.detailU[0], p.detailU[1], p.detailV[0],
          p.detailV[1], p.bloodColor[0], p.bloodColor[1], p.bloodColor[2], p.bloodRange[0], p.bloodRange[1]);
  fprintf(out, " | burn strength %g opacity %g", p.burnStrength, p.opacity);
  if (p.woundCombo) {
    fprintf(out, " | mapping scale %g second blend %g threshold %g", p.mappingScale, p.secondBlend, p.threshold);
    for (int e = 0; e < 2; ++e)
      fprintf(out, "\n   ellipsoid %d: centre (%g %g %g) x (%g %g %g) y (%g %g %g) z (%g %g %g)", e + 1, p.center[e][0],
              p.center[e][1], p.center[e][2], p.axisX[e][0], p.axisX[e][1], p.axisX[e][2], p.axisY[e][0],
              p.axisY[e][1], p.axisY[e][2], p.axisZ[e][0], p.axisZ[e][1], p.axisZ[e][2]);
    fprintf(out, "\n   projection scale (%g %g %g %g) bias (%g %g %g %g) | cut uv 1 (%g %g %g %g) 2 (%g %g %g %g)",
            p.projectionScale[0], p.projectionScale[1], p.projectionScale[2], p.projectionScale[3],
            p.projectionBias[0], p.projectionBias[1], p.projectionBias[2], p.projectionBias[3], p.cutScaleBias[0][0],
            p.cutScaleBias[0][1], p.cutScaleBias[0][2], p.cutScaleBias[0][3], p.cutScaleBias[1][0],
            p.cutScaleBias[1][1], p.cutScaleBias[1][2], p.cutScaleBias[1][3]);
  }
  fprintf(out, "\n");
  static const char *roles[ProbeDraw::kTextures] = {"base", "detail", "gradient", "cut", "burn"};
  for (int i = 0; i < ProbeDraw::kTextures; ++i) {
    if (draw.stages[i] < 0)
      continue;
    const void *texture = draw.textures[i];
    fprintf(out, "   s%d %s %p srgb %u", draw.stages[i], roles[i], texture, draw.srgb[i]);
    if (texture) {
      auto *base = static_cast<IDirect3DBaseTexture9 *>(const_cast<void *>(texture));
      D3DSURFACE_DESC desc = {};
      if (base->GetType() == D3DRTYPE_TEXTURE &&
          SUCCEEDED(static_cast<IDirect3DTexture9 *>(base)->GetLevelDesc(0, &desc)))
        fprintf(out, " %ux%u format %u levels %lu", desc.Width, desc.Height, unsigned(desc.Format),
                static_cast<unsigned long>(base->GetLevelCount()));
      fprintf(out, " hash %016llX%s", static_cast<unsigned long long>(draw.hashes[i]),
              draw.uploaded[i] ? " (runtime has it)" : draw.hashes[i] ? "" : " (no upload seen)");
      for (const auto &k : known)
        if (draw.hashes[i] && std::find(k.hashes.begin(), k.hashes.end(), draw.hashes[i]) != k.hashes.end())
          fprintf(out, " = %s", k.name);
    }
    fprintf(out, "\n");
  }
  fclose(out);
}

} // namespace proxy::l4d2::infected
