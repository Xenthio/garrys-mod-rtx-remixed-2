#pragma once

#include "x86_engine.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>

// Left 4 Dead 2 (x86): what only L4D2 has. Its engine side (install, markers,
// the map, cvars, anti-culling, the static prop skip, the F12 UI) is the x86
// engine side it shares with Portal 2 (x86_engine.h).
//
//  * l4d2_lights.cpp: the survivors' flashlights as Remix lights.
//  * l4d2_infected.cpp: the common infected's look and wounds (see infected).
struct remixapi_Interface;

namespace proxy::l4d2 {

// The module helpers (pe_module.h; Module, Pattern, ... as they were here).
using pe::Module;
using pe::Pattern;
using pe::bytesAt;
using pe::rttiVtable;
using pe::createInterface;
using pe::swapSlot;
using pe::writeCode;
using pe::writeJump;
using pe::envFlag;
using pe::envFloat;

// l4d2_lights.cpp. Main thread, once per frame (the main view's
// DrawWorldLists): reads the players' flashlights. Render thread, while the
// frame's lights are drawn: creates, updates and draws their Remix lights.
bool installLights();
void sampleLights();
void submitLights(const remixapi_Interface &api);
void dumpLights(FILE *out);

// l4d2_infected.cpp: the common infected (Source's Infected shader), drawn
// with their gradient-mapped look (remixapi_MaterialGradientMapEXT) and their
// wounds cut out (remixapi_InstanceInfoWoundCutEXT). Their draws are told
// apart by their shaders' constant tables (CTAB, read once per shader
// object): the pixel shader declares g_vGradSelect, g_vUVTransform,
// BaseTextureSampler and ColorLookupSampler; the wounded combination also
// g_tCutOutDecalMap and g_vWoundUVScaleBias1/2, its vertex shader the
// ellipsoids (g_vEllipsoidCenter...). Registers are taken from the tables.
// OPENREMIX_PROXY_INFECTED=0: drawn as any other model. Render thread.
namespace infected {
bool enabled();

// What one draw's shaders and constants say.
struct Params {
  // Samplers (D3D stages) of the base texture, detail (-1: none), gradient,
  // cut (-1: not the wounded combination) and burn (-1: not the burning one).
  int baseStage = 0, detailStage = -1, gradientStage = 5, cutStage = -1, burnStage = -1;
  float skinRow = 0, clothRow = 0, spriteOffset[2] = {}, detailU[2] = {1, 0}, detailV[2] = {0, 1};
  float bloodColor[3] = {1, 1, 1}, bloodRange[2] = {0, 1};
  // Burning: g_vShaderParams.x, 0 to 1 (0: not burning, or no burn sampler).
  // Opacity: g_DiffuseModulation's alpha, which the shader writes to oC0.w
  // (Source fades a model by it), 0 to 1 for an alpha-blended draw (else 1).
  // Both in steps of 1/kQuantum: they name the material, and animate.
  static constexpr float kQuantum = 64;
  float burnStrength = 0, opacity = 1;
  // Pixel shader registers, for the probe (-1: not declared): g_cEyeGlowColor,
  // g_DiffuseModulation, g_vShaderParams.
  int eyeGlowRegister = -1, modulationRegister = -1, shaderParamsRegister = -1;
  bool fadeable = false; // the pixel shader declares g_DiffuseModulation
  // A wound in place: the wounded combination of both shaders, and an
  // ellipsoid near the model (the game parks unused ones at 1e6).
  bool wounded = false;
  bool woundCombo = false; // the pixel shader is the wounded combination
  bool vertexShaderKnown = false; // the vertex shader's constant table names the registers
  float center[2][3] = {}, axisX[2][3] = {}, axisY[2][3] = {}, axisZ[2][3] = {};
  float mappingScale = 1, secondBlend = 0, projectionScale[4] = {}, projectionBias[4] = {};
  float cutScaleBias[2][4] = {}, threshold = 1;
};
// Whether the draw is an infected one (`out` filled). `ps`, `vs`: the
// shadowed float constants (224 / 256 registers); `alphaBlended`: the draw
// blends SRCALPHA, INVSRCALPHA (its opacity is then the modulation's alpha).
bool classify(const void *pixelShader, const void *vertexShader, const float *ps, const float *vs, bool alphaBlended,
              Params &out);

// Statistics.
enum Missing { kMissingBase, kMissingGradient, kMissingDetail, kMissingCut, kMissingBurn, kMissingCount };
void noteDraw(const Params &p, bool material, bool woundSent);
// An infected shader's draw not taken as infected: blended (other than a
// fade), alpha tested, lifted or a particle.
void noteRefused(bool blend, bool alphaTest, bool lifted, bool particle, const Params &p);
void noteMissing(Missing which);
void noteMaterialCreated(bool failed);
void dumpStatistics(FILE *out, size_t materials);

// Dev aid: bin/shaderapi_proxy_infected.request (its contents: a draw count,
// default 32; "wounded [count]"; "refused [count]": the infected shaders'
// draws not taken) writes the next infected draws to bin/infected_probe.txt.
// servicePresent: once per frame (polls the request); probing: draws left.
void servicePresent(const wchar_t *directory, uint64_t frame);
bool probing();
struct ProbeDraw {
  // Base, detail, gradient, cut, burn: their stages (-1: none), textures,
  // runtime texture hashes (0: none), whether the runtime has them, and their
  // samplers' D3DSAMP_SRGBTEXTURE.
  static constexpr int kTextures = 5;
  int stages[kTextures] = {-1, -1, -1, -1, -1};
  const void *textures[kTextures] = {};
  uint64_t hashes[kTextures] = {};
  bool uploaded[kTextures] = {};
  uint32_t srgb[kTextures] = {};
  const void *pixelShader = nullptr;
  uint32_t vertices = 0, primitives = 0;
  uint32_t bonesPerVertex = 0, boneCount = 0; // as the mesh was built (boneCount 1: rigid)
  bool skinnedLayout = false, dynamicVertices = false, flexIgnored = false;
  bool material = false, woundSent = false;
  // The draw's blend and alpha test state.
  bool blend = false, separateAlpha = false, alphaTest = false;
  uint32_t srcBlend = 0, dstBlend = 0, blendOp = 0, srcBlendAlpha = 0, dstBlendAlpha = 0, blendOpAlpha = 0;
  uint32_t alphaFunc = 0, alphaRef = 0;
  // Not taken (probed under "refused" only), and why besides its blend state.
  bool refused = false, lifted = false, particle = false;
  uint64_t frame = 0;
};
void probe(const Params &p, const float *ps, const float *vs, const ProbeDraw &draw);
} // namespace infected

} // namespace proxy::l4d2
