#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

// What the Source material of a draw does with its base texture besides
// being lit: unlit shaders show it as it is, self-illuminated materials
// ($selfillum) show the masked part of it as it is. Read from the engine's
// material at IShaderAPI::Bind (the material system binds a material, after
// its proxies ran, before the draws that use it), so values that material
// proxies animate ($selfillumtint following the player's weapon colour, $color
// flickers) are current for every draw.
namespace proxy::engine_materials {

struct Shading {
  enum Mode : uint8_t {
    Lit,                // nothing emitted
    Unlit,              // the whole surface: texture x tint (x vertex colour)
    SelfIllumBaseAlpha, // $selfillum: masked by the base texture's alpha
    SelfIllumMask,      // $selfillummask: masked by that texture's colour
    SelfIllumMaskAlpha, // $selfillum_envmapmask_alpha: by the $envmapmask's alpha
  };
  Mode mode = Lit;
  bool vertexColor = false, vertexAlpha = false; // $vertexcolor, $vertexalpha (unlit)
  // The VMT marks an unlit material as luminous ($selfillum on UnlitGeneric,
  // which the shader ignores): BSP materials only.
  bool luminous = false;
  float tint[3] = {1, 1, 1}; // linear multiplier of the emitted colour
  float maskScale = 1;       // emitted share = mask * maskScale (above one: overbright)
  std::string mask;          // mask texture ("models/x/y_mask"), SelfIllumMask / MaskAlpha
  std::string base;          // $basetexture's name ("" when unknown), also of Lit materials
};

// The shading a VMT's shader name selects: Unlit for the unlit shaders,
// SelfIllumBaseAlpha for the shaders that implement $selfillum (the caller
// checks the material's flags and masks), else Lit.
Shading::Mode shaderMode(const char *shaderName);

// Emitted share of a $selfillumfresnel material, averaged over the directions
// a surface is seen from ($selfillumfresnelminmaxexp).
float fresnelShare(float minimum, float maximum, float exponent);

// Validates the engine's material classes and IShaderAPI::Bind (the real
// shader API's vtable). False: nothing is read, every draw is Lit.
bool install(const void *const *shaderApiVtable);

// IShaderAPI::Bind(IMaterial *), after the engine's (render thread).
void onBind(void *material);

// The bound material's shading (render thread; valid until the next onBind).
const Shading &current();
// Its name, for statistics ("" when unknown).
const char *currentName();

void dumpStatistics(FILE *out);

} // namespace proxy::engine_materials
