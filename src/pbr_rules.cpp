#include "pbr_rules.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <tuple>

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
// A base texture darker than this (mean luminance) under an envmap is metal.
constexpr float kEnvmapMetalLuminance = 0.35f;
// ... and its reflection this strong (mean $envmaptint, as authored).
constexpr float kEnvmapMetalTint = 0.7f;
float phongRoughness(float exponent, float boost) {
  float alpha = std::sqrt(2 / (kPhongToBlinn * std::clamp(exponent, 1.0f, 150.0f) + 2));
  if (boost > 1)
    alpha = std::min(alpha, 0.5f / std::sqrt(boost));
  return std::clamp(std::sqrt(alpha), 0.04f, 1.0f);
}

} // namespace

bool capable(const std::string &shader) {
  for (const char *name : {"vertexlitgeneric", "lightmappedgeneric", "worldvertextransition", "screenspace_general"})
    if (shader.rfind(name, 0) == 0)
      return true;
  return false;
}

Material convert(const Inputs &given) {
  Material out;
  if (!capable(given.shader))
    return out;
  // Phong is drawn by the shaders that have its parameters: VertexLitGeneric
  // and (in GMod) LightmappedGeneric, not WorldVertexTransition. Blends
  // written for later branches (zs_verystill_v1a's forest/blend_*:
  // WorldVertexTransition with $phong 1, $phongexponent 170 under a 0.01
  // $envmaptint) read as glossy from their VMT, where the game shows them
  // matte (the world's materials come from the VMT text, bsp_world.cpp, not
  // from the shader's parameters).
  Inputs in = given;
  if (in.shader.rfind("vertexlitgeneric", 0) != 0 && in.shader.rfind("lightmappedgeneric", 0) != 0) {
    in.phong = false;
    in.phongExponentTexture.clear();
    in.baseAlphaPhongMask = false;
  }

  // --- ExoPBR: ARM texture (Red = AO, Green = Roughness, Blue = Metalness) ---
  if (in.isExoPbr || (!in.arm.empty() && isScreenspacePbr(in.shader))) {
    out.active = true;
    out.normal = in.bump;
    out.ssbump = false;
    if (!in.arm.empty()) {
      // Roughness: Green (1)
      out.maps[0].texture = in.arm;
      out.maps[0].channel = 1;
      out.maps[0].invert = false;
      out.maps[0].curve = 0;
      out.maps[0].exponent = 1.0f;
      out.maps[0].bias = 0.0f;
      out.maps[0].scale = 1.0f;
      out.roughness = 1.0f;

      // Metalness: Blue (2)
      out.maps[1].texture = in.arm;
      out.maps[1].channel = 2;
      out.maps[1].invert = false;
      out.maps[1].curve = 0;
      out.maps[1].exponent = 1.0f;
      out.maps[1].bias = 0.0f;
      out.maps[1].scale = 1.0f;
      out.metallic = 1.0f;
    } else {
      out.roughness = 0.5f;
      out.metallic = 0.0f;
    }
    return out;
  }

  if (in.bump.empty() && !in.phong && !in.envmap && in.envmapMask.empty() && in.phongExponentTexture.empty())
    return out;
  out.active = true;
  out.normal = in.bump;
  out.ssbump = in.ssbump && !in.bump.empty();
  out.directSpecular = in.phong || in.mwb;
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
    // $envmaptint is the reflection's strength, which a dielectric's cannot
    // follow (Fresnel sets it): a faint one is a rough sheen. ToPBR's 0.6 -
    // 0.4 x tint left gm_scrapland's dried mud ($envmaptint 0.34, unmasked)
    // a glossy 0.47 that mirrored the low sun across the ground (issue #84);
    // as strong a tint as Source allows stays as smooth (0.2).
    roughness = in.hasEnvmapTint ? 1 - 0.8f * (in.envmapTint[0] + in.envmapTint[1] + in.envmapTint[2]) / 3 : 0.4f;
  } else if (in.envmapMask.empty()) {
    // Only a bump map: Source draws it diffuse, without any highlight.
    // ToPBR's 0.5 put a sun glare on HL2's bumped rock and gravel
    // (d2_coast_03's nature/blendrockgravel001a).
    roughness = 1;
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
  // A dark base under a cubemap reflection (no Phong): Source draws it as the
  // reflection over little diffuse, which is how polished metal looks
  // (Portal's chamber pipes, metal/metal_lift001: $basealphaenvmapmask,
  // $envmaptint 0.8; GMod issue #97). As ToPBR: where the mask reflects it
  // is metal, and the metal is $envmaptint (gamma 2.2 in Source's
  // shaders) instead of the dark base. A light (painted) base stays a shiny
  // dielectric, and so does a weaker reflection: a sheen over a textured
  // panel (the same map's metal/metalwall048b, $envmaptint 0.6 0.5 0.5, went
  // flat grey as metal). A mask in the normal map's alpha is a bumped
  // surface's gloss, not polished metal either (HL2's train seats,
  // $envmaptint 0.8, turned to chrome).
  if (metal.texture.empty() && !out.metallic && !in.phong && in.envmap && !in.normalAlphaEnvmapMask &&
      envmapMetal() && in.luminance && !in.base.empty()) {
    const float luminance = in.luminance(in.base);
    float tint[3] = {1, 1, 1};
    if (in.hasEnvmapTint)
      for (int c = 0; c < 3; ++c)
        tint[c] = std::pow(std::clamp(in.envmapTint[c], 0.0f, 1.0f), 2.2f);
    const float strength = in.hasEnvmapTint ? (in.envmapTint[0] + in.envmapTint[1] + in.envmapTint[2]) / 3 : 1;
    if (luminance >= 0 && luminance < kEnvmapMetalLuminance && strength >= kEnvmapMetalTint) {
      // The mask as rough reads it (smooth where it reflects), turned around,
      // is the metal. Source's cubemap is as sharp under a faint mask as a
      // full one, only weaker: a mirror (roughness 0) beside fully rough rust
      // (1), switching to metal past ToPBR's 30%, made the pipes blotchy at
      // every rust spot. Here the mask is metal as it is, roughness 0.05 to
      // 0.8 along the mask's rough side (OPENREMIX_PROXY_PBR_ENVMAP_METAL_
      // ROUGHNESS="min max exponent"): bare metal stays glossy, just above
      // where the runtime's primary surface replacement takes smooth metal
      // as an exact mirror (0.041), whose hard edge against the glossier
      // texels around it cut across the pipes;
      // and roughness reads the mask two mips blurred: Valve paints it in
      // crisp patches, whose edges a sharp path-traced reflection showed as
      // blotches where Source's dim cubemap hid them
      // (OPENREMIX_PROXY_PBR_ENVMAP_METAL_BLUR, in mips; 0: sharp). Metallic
      // reads it sharp: it decides where the rust's diffuse shows, whose
      // detail a blurred one washed out.
      const bool masked = !rough.texture.empty();
      if (masked) {
        metal.texture = rough.texture;
        metal.channel = rough.channel;
        metal.invert = !rough.invert;
        const auto [low, high, exponent] = envmapMetalRoughness();
        rough.bias = low;
        rough.scale = high - low;
        rough.exponent = exponent;
        out.blur = envmapMetalBlur();
      } else {
        out.metallic = 1;
      }
      out.metalColored = true;
      std::copy(tint, tint + 3, out.metalColor);
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

std::tuple<float, float, float> envmapMetalRoughness() {
  static const std::tuple<float, float, float> range = [] {
    char v[64] = {};
    float low = 0.05f, high = 0.8f, exponent = 1;
    if (GetEnvironmentVariableA("OPENREMIX_PROXY_PBR_ENVMAP_METAL_ROUGHNESS", v, sizeof(v))) {
      float a, b, c = 1;
      if (std::sscanf(v, "%f %f %f", &a, &b, &c) >= 2 || std::sscanf(v, "%f,%f,%f", &a, &b, &c) >= 2) {
        low = std::clamp(a, 0.0f, 1.0f);
        high = std::clamp(b, low, 1.0f);
        exponent = std::clamp(c, 0.0f, 15.0f);
      }
    }
    return std::tuple{low, high, exponent};
  }();
  return range;
}

float envmapMetalBlur() {
  static const float blur = [] {
    char v[32] = {};
    return GetEnvironmentVariableA("OPENREMIX_PROXY_PBR_ENVMAP_METAL_BLUR", v, sizeof(v))
               ? std::clamp(float(std::atof(v)), 0.0f, 15.0f)
               : 2.0f;
  }();
  return blur;
}

bool envmapMetal() {
  static const bool on = [] {
    char v[8] = {};
    return !(GetEnvironmentVariableA("OPENREMIX_PROXY_PBR_ENVMAP_METAL", v, sizeof(v)) && v[0] == '0');
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
