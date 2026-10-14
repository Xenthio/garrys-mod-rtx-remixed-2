#include "pbr_rules.h"

#include <windows.h>

#include <algorithm>
#include <cmath>

namespace proxy::pbr {
namespace {

// Source's Phong exponent n as a GGX lobe. Its shaders raise R.L (the eye's
// reflection against the light), a lobe as wide as Blinn-Phong's N.H at
// about four times the exponent; Blinn-Phong m is GGX at alpha =
// sqrt(2 / (m + 2)), and the runtime's perceptual roughness is sqrt(alpha).
// (kPhongToBlinn: also in the exponent texture's curve.) Source's Phong is not
// energy conserving: its peak is $phongboost x the Fresnel term whatever the
// lobe's width, against a diffuse of albedo x light. A GGX peak is that
// bright beside a Lambert diffuse at alpha = 1 / (2 sqrt(boost)) (at normal
// incidence: F / (4 alpha^2) = boost x F); the sharper of the two keeps a
// boosted highlight as bright.
constexpr float kPhongToBlinn = 4;
float phongRoughness(float exponent, float boost) {
  float alpha = std::sqrt(2 / (kPhongToBlinn * std::clamp(exponent, 1.0f, 150.0f) + 2));
  if (boost > 1)
    alpha = std::min(alpha, 0.5f / std::sqrt(boost));
  return std::clamp(std::sqrt(alpha), 0.04f, 1.0f);
}

} // namespace

bool capable(const std::string &shader) {
  for (const char *name : {"vertexlitgeneric", "lightmappedgeneric", "worldvertextransition"})
    if (shader.rfind(name, 0) == 0)
      return true;
  return false;
}

Material convert(const Inputs &in) {
  Material out;
  if (!capable(in.shader) ||
      (in.bump.empty() && !in.phong && !in.envmap && in.envmapMask.empty() && in.phongExponentTexture.empty()))
    return out;
  out.active = true;
  out.normal = in.bump;
  out.ssbump = in.ssbump && !in.bump.empty();
  // An unmasked sphere map in mode 1 is how Source draws chrome.
  if (in.envmap && in.envmapSphere && in.envmapMode) {
    out.roughness = 0.05f;
    out.metallic = 1;
    return out;
  }
  float roughness = 0.5f;
  if (in.phong) {
    if (in.phongExponent > 0)
      roughness = phongRoughness(in.phongExponent, in.phongBoost);
  } else if (in.envmap) {
    roughness = in.hasEnvmapTint ? 0.6f - 0.4f * (in.envmapTint[0] + in.envmapTint[1] + in.envmapTint[2]) / 3 : 0.4f;
  }
  out.roughness = roughness = std::clamp(roughness, 0.04f, 1.0f);
  Map &rough = out.maps[0], &metal = out.maps[1];
  bool phongMask = false; // rough reads the Phong mask
  // Metal, where the material encodes it. Pseudo-PBR weapon and model packs
  // (MWB PBR Gen, BlueFlyTrap) mark metal in the base alpha and remove its
  // diffuse there ($color2 near black, $blendtintbybasealpha), leaving the
  // albedo-tinted highlight to colour it; Source's own models mark where the
  // highlight takes the albedo's colour ($phongalbedotint) in the exponent
  // texture's green. Either is what metal looks like.
  const bool darkColor2 = in.blendTintByBaseAlpha && in.hasColor2 &&
                          std::max({in.color2[0], in.color2[1], in.color2[2]}) < 0.3f && !in.base.empty();
  if (darkColor2) {
    metal.texture = in.base;
    metal.channel = 3;
    out.metalByBaseAlpha = true;
  } else if (in.phong && in.phongAlbedoTint && !in.phongExponentTexture.empty()) {
    metal.texture = in.phongExponentTexture;
    metal.channel = 1;
  }
  if (in.phong) {
    if (!in.phongExponentTexture.empty()) {
      rough.texture = in.phongExponentTexture;
      rough.bias = 1;
      if (darkColor2) {
        // The packs store gloss: BlueFlyTrap in red; MWB PBR Gen in alpha,
        // and its fourth power in red for Source's exponent (some of its
        // outputs leave red 0, Source's dullest exponent).
        rough.channel = in.mwb ? 3 : 0;
        rough.scale = -1;
      } else {
        // Its red channel is the exponent: 1 + 149 x red (in Blinn-Phong
        // terms, x kPhongToBlinn).
        rough.curve = 1;
        rough.bias = kPhongToBlinn;
        rough.scale = 149 * kPhongToBlinn;
      }
    } else if (in.baseAlphaPhongMask || !in.bump.empty()) {
      // The Phong mask scales the highlight: none is rough, full the exponent's.
      rough.texture = in.baseAlphaPhongMask ? in.base : in.bump;
      rough.channel = 3;
      phongMask = true;
      rough.bias = 1;
      rough.scale = roughness - 1;
    }
  } else if (!in.envmapMask.empty()) {
    // Reflection strength: a reflective texel is smooth.
    rough.texture = in.envmapMask;
    rough.invert = true;
  } else if (in.baseAlphaEnvmapMask && !in.base.empty()) {
    rough.texture = in.base;
    rough.channel = 3;
  } else if (in.normalAlphaEnvmapMask && !in.bump.empty()) {
    rough.texture = in.bump;
    rough.channel = 3;
    rough.invert = true;
    if (out.ssbump) {
      // An SSBump's mask says where it reflects, not how sharply: present
      // stays polished (0.25), and only a faint mask turns rough.
      rough.exponent = 3;
      rough.bias = 0.25f;
      rough.scale = 0.75f;
    }
  }
  // A dark, Phong-lit material whose surface is metal ($surfaceprop): Source
  // shows it as its highlight on a dark base, which is how metal looks (HL2's
  // helicopter bomb). Under its Phong mask; a light (painted) one stays
  // dielectric.
  if (metal.texture.empty() && !out.metallic && in.phong && phongMetal() &&
      in.surfaceProp.find("metal") != std::string::npos && in.luminance && !in.base.empty()) {
    const float luminance = in.luminance(in.base);
    if (luminance >= 0 && luminance < 0.35f) {
      if (phongMask) {
        // The mask scales the highlight's strength, not its width: here the
        // metal (a reflection's strength), while the lobe keeps the exponent's.
        metal.texture = rough.texture;
        metal.channel = 3;
        rough = Map{};
      } else if (rough.texture.empty()) {
        out.metallic = 1;
      }
    }
  }
  return out;
}

bool mwbBase(const std::string &base) {
  return (base.size() > 4 && base.compare(base.size() - 4, 4, "_rgb") == 0) || base.find("pbr/output/") != std::string::npos;
}

bool phongMetal() {
  static const bool on = [] {
    char v[8] = {};
    return !(GetEnvironmentVariableA("OPENREMIX_PROXY_PBR_PHONG_METAL", v, sizeof(v)) && v[0] == '0');
  }();
  return on;
}

bool enabled() {
  static const bool on = [] {
    char v[8] = {};
    return !(GetEnvironmentVariableA("OPENREMIX_PROXY_PBR", v, sizeof(v)) && v[0] == '0');
  }();
  return on;
}

} // namespace proxy::pbr
