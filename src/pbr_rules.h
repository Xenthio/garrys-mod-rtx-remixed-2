#pragma once

#include <string>
#include <tuple>

// Source materials as PBR materials, converted while they are drawn: the
// runtime reads the game's own textures (its normal map, and the masks Source
// reads specular strength or exponent from) through a per-map channel and
// curve (remixapi_MaterialPbrRemapEXT), so nothing is generated. The rules
// follow ToPBR's for standard Source materials (garrys-mod-rtx-remixed,
// material_pipeline/to_pbr/format_source.cpp).
namespace proxy::pbr {

// What a material says, from its live parameters (engine_materials) or its
// VMT (bsp_world). Texture names are lowercase ("" when none).
struct Inputs {
  std::string shader; // lowercase
  std::string base, bump, envmapMask, phongExponentTexture;
  bool ssbump = false, phong = false, envmap = false;
  bool baseAlphaPhongMask = false, baseAlphaEnvmapMask = false, normalAlphaEnvmapMask = false;
  bool envmapSphere = false, envmapMode = false;
  float phongExponent = 0; // 0: not given
  bool hasEnvmapTint = false;
  float envmapTint[3] = {1, 1, 1};
  // Pseudo-PBR encodings: $phongalbedotint, $blendtintbybasealpha, $color2
  // (gamma), and a marker of MWB PBR Gen's output (an "_rgb" base texture, or
  // its envmap tint proxies).
  bool phongAlbedoTint = false, blendTintByBaseAlpha = false, hasColor2 = false, mwb = false;
  float color2[3] = {1, 1, 1};
  float phongBoost = 1;
  // ExoPBR (screenspace_general_8tex)
  std::string arm;      // ExoPBR $texture1 (R=AO, G=Rough, B=Metal)
  bool isExoPbr = false;
  // $surfaceprop (lowercase), and the base texture's mean luminance (-1
  // unknown): asked for (luminance()) only by the dark Phong metal rule.
  std::string surfaceProp;
  float (*luminance)(const std::string &texture) = nullptr;
};

// The texture a value is read from, and how (remixapi_MaterialPbrChannelEXT).
struct Map {
  std::string texture; // "" none: the constant
  unsigned channel = 0, curve = 0;
  bool invert = false;
  float exponent = 1, bias = 0, scale = 1;
};

// The material's PBR description. Inactive: the material has nothing to
// convert (no normal map, Phong or reflection) and keeps diffuse shading.
struct Material {
  bool active = false;
  bool directSpecular = true; // Legacy cubemaps alone do not author a direct lobe.
  std::string normal; // its normal map
  bool ssbump = false;
  Map maps[2]; // roughness, metallic
  float roughness = 1, metallic = 0;
  // The base texture's alpha is a metal mask whose diffuse the material's
  // $color2 removes: the draw's tint is not applied (the metal's colour is
  // the base texture's).
  bool metalByBaseAlpha = false;
  // The metal's colour (remixapi_MaterialMetalColorEXT, linear): where it is
  // metal the albedo turns to it (envmapped metal, see convert()).
  bool metalColored = false;
  float metalColor[3] = {1, 1, 1};
  // The roughness map's least mip (remixapi_MaterialPbrBlurEXT; 0: as sharp
  // as the view allows).
  float blur = 0;
};

Material convert(const Inputs &in);
// The base texture is MWB PBR Gen's output ("_rgb", or in "pbr/output/").
bool mwbBase(const std::string &base);
// The shader is one convert() takes (the lit surface shaders); lowercase.
bool capable(const std::string &shader);
inline bool isScreenspacePbr(const std::string &shader) {
  return shader.rfind("screenspace_general", 0) == 0;
}

// OPENREMIX_PROXY_PBR=0 turns the conversion off.
bool enabled();
// OPENREMIX_PROXY_PBR_PHONG_METAL=0 turns the dark Phong metal rule off.
bool phongMetal();
// OPENREMIX_PROXY_PBR_ENVMAP_METAL=0 turns the dark envmapped metal rule off.
bool envmapMetal();
// The roughness its mask maps to: min, max and the mask's exponent
// (OPENREMIX_PROXY_PBR_ENVMAP_METAL_ROUGHNESS).
std::tuple<float, float, float> envmapMetalRoughness();
// The mips its mask is blurred by (OPENREMIX_PROXY_PBR_ENVMAP_METAL_BLUR).
float envmapMetalBlur();

} // namespace proxy::pbr
