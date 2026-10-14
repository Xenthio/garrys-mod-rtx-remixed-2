#pragma once

#include <string>

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
  std::string normal; // its normal map
  bool ssbump = false;
  Map maps[2]; // roughness, metallic
  float roughness = 1, metallic = 0;
  // The base texture's alpha is a metal mask whose diffuse the material's
  // $color2 removes: the draw's tint is not applied (the metal's colour is
  // the base texture's).
  bool metalByBaseAlpha = false;
};

Material convert(const Inputs &in);
// The base texture is MWB PBR Gen's output ("_rgb", or in "pbr/output/").
bool mwbBase(const std::string &base);
// The shader is one convert() takes (the lit surface shaders); lowercase.
bool capable(const std::string &shader);

// OPENREMIX_PROXY_PBR=0 turns the conversion off.
bool enabled();
// OPENREMIX_PROXY_PBR_PHONG_METAL=0 turns the dark Phong metal rule off.
bool phongMetal();

} // namespace proxy::pbr
