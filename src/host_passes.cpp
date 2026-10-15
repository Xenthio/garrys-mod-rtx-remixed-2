#include "host_passes.h"
#include "remix_output.h"
#include "d3d_hooks.h"
#include "dxvk_hash.h"
#include "engine_materials.h"
#include "texture_formats.h"
#include "dxso/dxso_module.h"
#include "dxso/dxso_modinfo.h"
#include "dxso/dxso_util.h"
#include "d3d9/d3d9_caps.h"
#include "d3d9/d3d9_spec_constants.h"
#include "d3d9/d3d9_state.h"
#include <remix/remix_c.h>
#include <remix/remix_native_ext.h>
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace proxy::hostpass {
namespace {
// The runtime's extension, when it has it.
PFN_remixapi_RegisterHostShaderEXT g_registerShader = nullptr;
PFN_remixapi_SubmitHostPassesEXT g_submit = nullptr;
PFN_remixapi_GetHostPassStatisticsEXT g_statistics = nullptr;
PFN_remixapi_UpdateHostTargetEXT g_updateTarget = nullptr;
PFN_remixapi_UpdateHostVolumeEXT g_updateVolume = nullptr; // may be absent
PFN_remixapi_GetHostPassesEnabledEXT g_passesEnabled = nullptr;
bool g_enabled = false;

// VkFormat numbers the API takes.
constexpr uint32_t kFormatR32Sfloat = 100, kFormatR32G32Sfloat = 103, kFormatR32G32B32Sfloat = 106,
                   kFormatR32G32B32A32Sfloat = 109, kFormatB8G8R8A8Unorm = 44, kFormatR8G8B8A8Uint = 41,
                   kFormatR8G8B8A8Unorm = 37, kFormatR16G16Sint = 82, kFormatR16G16B16A16Sint = 96,
                   kFormatR16G16Snorm = 78, kFormatR16G16B16A16Snorm = 92, kFormatR16G16Unorm = 77,
                   kFormatR16G16B16A16Unorm = 91, kFormatR16G16Sfloat = 83, kFormatR16G16B16A16Sfloat = 97,
                   kFormatR16Sfloat = 76, kFormatA2B10G10R10UnormPack32 = 64, kFormatR8Unorm = 9;

uint32_t vertexFormat(BYTE type) {
  switch (type) {
  case D3DDECLTYPE_FLOAT1: return kFormatR32Sfloat;
  case D3DDECLTYPE_FLOAT2: return kFormatR32G32Sfloat;
  case D3DDECLTYPE_FLOAT3: return kFormatR32G32B32Sfloat;
  case D3DDECLTYPE_FLOAT4: return kFormatR32G32B32A32Sfloat;
  case D3DDECLTYPE_D3DCOLOR: return kFormatB8G8R8A8Unorm;
  case D3DDECLTYPE_UBYTE4: return kFormatR8G8B8A8Uint;
  case D3DDECLTYPE_SHORT2: return kFormatR16G16Sint;
  case D3DDECLTYPE_SHORT4: return kFormatR16G16B16A16Sint;
  case D3DDECLTYPE_UBYTE4N: return kFormatR8G8B8A8Unorm;
  case D3DDECLTYPE_SHORT2N: return kFormatR16G16Snorm;
  case D3DDECLTYPE_SHORT4N: return kFormatR16G16B16A16Snorm;
  case D3DDECLTYPE_USHORT2N: return kFormatR16G16Unorm;
  case D3DDECLTYPE_USHORT4N: return kFormatR16G16B16A16Unorm;
  case D3DDECLTYPE_UDEC3: return kFormatA2B10G10R10UnormPack32;
  case D3DDECLTYPE_FLOAT16_2: return kFormatR16G16Sfloat;
  case D3DDECLTYPE_FLOAT16_4: return kFormatR16G16B16A16Sfloat;
  default: return 0;
  }
}

// Render-target formats (0: the frame's).
uint32_t targetFormat(uint32_t d3d) {
  switch (d3d) {
  case D3DFMT_A8R8G8B8:
  case D3DFMT_X8R8G8B8: return kFormatB8G8R8A8Unorm;
  case D3DFMT_A8B8G8R8:
  case D3DFMT_X8B8G8R8: return kFormatR8G8B8A8Unorm;
  case D3DFMT_A16B16G16R16F: return kFormatR16G16B16A16Sfloat;
  case D3DFMT_A32B32G32R32F: return kFormatR32G32B32A32Sfloat;
  case D3DFMT_G16R16F: return kFormatR16G16Sfloat;
  case D3DFMT_R16F: return kFormatR16Sfloat;
  case D3DFMT_R32F: return kFormatR32Sfloat;
  case D3DFMT_A16B16G16R16: return kFormatR16G16B16A16Unorm;
  case D3DFMT_L8: return kFormatR8Unorm;
  case D3DFMT_A8L8: return kFormatR8G8B8A8Unorm;
  default: return 0;
  }
}
// How its channels read (luminance formats, alpha-less ones).
uint32_t targetChannels(uint32_t d3d) {
  switch (d3d) {
  case D3DFMT_L8: return REMIXAPI_HOST_CHANNELS_LUMINANCE_EXT;
  case D3DFMT_A8L8: return REMIXAPI_HOST_CHANNELS_LUMINANCE_ALPHA_EXT;
  case D3DFMT_X8R8G8B8:
  case D3DFMT_X8B8G8R8: return REMIXAPI_HOST_CHANNELS_NO_ALPHA_EXT;
  default: return REMIXAPI_HOST_CHANNELS_DEFAULT_EXT;
  }
}

uint32_t blendFactor(uint32_t d3d) { // D3DBLEND -> VkBlendFactor
  switch (d3d) {
  case D3DBLEND_ZERO: return 0;
  case D3DBLEND_ONE: return 1;
  case D3DBLEND_SRCCOLOR: return 2;
  case D3DBLEND_INVSRCCOLOR: return 3;
  case D3DBLEND_SRCALPHA: return 6;
  case D3DBLEND_INVSRCALPHA: return 7;
  case D3DBLEND_DESTALPHA: return 8;
  case D3DBLEND_INVDESTALPHA: return 9;
  case D3DBLEND_DESTCOLOR: return 4;
  case D3DBLEND_INVDESTCOLOR: return 5;
  case D3DBLEND_SRCALPHASAT: return 14;
  case D3DBLEND_BLENDFACTOR: return 10;
  case D3DBLEND_INVBLENDFACTOR: return 11;
  default: return 1;
  }
}
uint32_t blendOp(uint32_t d3d) { return d3d >= D3DBLENDOP_ADD && d3d <= D3DBLENDOP_MAX ? d3d - 1 : 0; }

// A shader compiled and registered with the runtime, by its bytecode's hash
// (a shader object's address is reused after its release).
struct Shader {
  remixapi_HostShaderHandleEXT handle = 0;
  uint64_t hash = 0;
  bool vertex = false;
  uint32_t inputMask = 0;
  // A vertex shader's inputs: the location dxso gave each declared semantic
  // (its declaration's order), which the vertex declaration's elements map to.
  struct Input {
    uint32_t usage, usageIndex, location;
  };
  std::vector<Input> inputs;
  std::vector<remixapi_HostShaderBindingEXT> bindings;
  bool unsupported = false; // a storage buffer binding, or no compile
};
std::unordered_map<uint64_t, Shader> g_shaders;

// Plain textures sampled by passes, uploaded as host targets, by the version
// of their contents uploaded.
struct Uploaded {
  uint32_t version = 0, width = 0, height = 0;
};
std::unordered_map<const void *, Uploaded> g_uploaded;

struct Statistics {
  uint32_t draws = 0, copies = 0, clears = 0, compiled = 0, failed = 0, uploads = 0, depth = 0, dropped = 0;
  uint32_t noShader = 0, plainTexture = 0, noData = 0, badPrimitive = 0, pruned = 0;
  uint32_t enginePost = 0; // Source's bloom / HDR passes dropped
} g_stats;

// The proxy's own shaders (src/shaders): the view-depth pass.
#include "host_depth.vert.inc"
#include "host_depth.frag.inc"
#include "host_frame_depth.frag.inc"
remixapi_HostShaderHandleEXT g_depthVertex = 0, g_depthFragment = 0, g_frameDepthFragment = 0;
bool g_depthShadersFailed = false;
// The last clear colour of each render-target texture (ARGB): what the
// engine's depth pass leaves where nothing is drawn.
std::unordered_map<const void *, uint32_t> g_clearColor;
// Targets whose depth pass was replaced this frame.
std::unordered_map<const void *, uint64_t> g_depthDone;

// A 2D texture's level 0 read through D3D (a managed texture the proxy keeps
// no CPU copy of: large compressed ones): RGBA or BGRA, 8 bits a channel.
bool lockPixels(const void *object, uint32_t &width, uint32_t &height, bool &bgra, std::vector<uint8_t> &pixels,
                char why[64]) {
  auto *texture = const_cast<IDirect3DTexture9 *>(static_cast<const IDirect3DTexture9 *>(object));
  D3DSURFACE_DESC desc = {};
  if (FAILED(texture->GetLevelDesc(0, &desc)) || !desc.Width || !desc.Height ||
      uint64_t(desc.Width) * desc.Height > 4096u * 4096u) {
    snprintf(why, 64, "no level description or too large");
    return false;
  }
  const bool wide = desc.Format == D3DFMT_A8R8G8B8 || desc.Format == D3DFMT_X8R8G8B8;
  const bool bc1 = desc.Format == D3DFMT_DXT1, bc3 = desc.Format == D3DFMT_DXT5;
  const bool luminance = desc.Format == D3DFMT_L8, luminanceAlpha = desc.Format == D3DFMT_A8L8;
  if (!wide && !bc1 && !bc3 && !luminance && !luminanceAlpha) {
    snprintf(why, 64, "format %u pool %u", unsigned(desc.Format), unsigned(desc.Pool));
    return false;
  }
  D3DLOCKED_RECT locked = {};
  if (const HRESULT result = texture->LockRect(0, &locked, nullptr, D3DLOCK_READONLY); FAILED(result) || !locked.pBits) {
    snprintf(why, 64, "lock %08lx, format %u pool %u usage %lx", static_cast<unsigned long>(result),
             unsigned(desc.Format), unsigned(desc.Pool), static_cast<unsigned long>(desc.Usage));
    return false;
  }
  width = desc.Width;
  height = desc.Height;
  bgra = wide;
  const auto *source = static_cast<const uint8_t *>(locked.pBits);
  if (wide) {
    pixels.resize(size_t(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y)
      std::memcpy(pixels.data() + size_t(y) * width * 4, source + size_t(y) * locked.Pitch, size_t(width) * 4);
    if (desc.Format == D3DFMT_X8R8G8B8)
      for (size_t i = 3; i < pixels.size(); i += 4)
        pixels[i] = 255;
  } else if (luminance || luminanceAlpha) {
    const uint32_t bytes = luminance ? 1 : 2;
    pixels.resize(size_t(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y)
      for (uint32_t x = 0; x < width; ++x) {
        const uint8_t *texel = source + size_t(y) * locked.Pitch + size_t(x) * bytes;
        uint8_t *out = pixels.data() + (size_t(y) * width + x) * 4;
        out[0] = out[1] = out[2] = texel[0];
        out[3] = luminance ? 255 : texel[1];
      }
  } else {
    const uint32_t blockBytes = bc1 ? 8 : 16, rowBytes = ((width + 3) / 4) * blockBytes, rows = (height + 3) / 4;
    std::vector<uint8_t> blocks(size_t(rowBytes) * rows);
    for (uint32_t y = 0; y < rows; ++y)
      std::memcpy(blocks.data() + size_t(y) * rowBytes, source + size_t(y) * locked.Pitch, rowBytes);
    pixels = bc1 ? formats::decodeBC1(blocks.data(), width, height) : formats::decodeBC3(blocks.data(), width, height);
  }
  texture->UnlockRect(0);
  return pixels.size() == size_t(width) * height * 4;
}

// A volume texture's level 0, likewise (BGRA formats), through D3D.
bool lockVolume(const void *object, uint32_t &width, uint32_t &height, uint32_t &depth, std::vector<uint8_t> &pixels,
                char why[64]) {
  auto *texture = const_cast<IDirect3DVolumeTexture9 *>(static_cast<const IDirect3DVolumeTexture9 *>(object));
  D3DVOLUME_DESC desc = {};
  if (FAILED(texture->GetLevelDesc(0, &desc)) || !desc.Width || !desc.Height || !desc.Depth ||
      uint64_t(desc.Width) * desc.Height * desc.Depth > 512u * 512u * 512u) {
    snprintf(why, 64, "no level description or too large");
    return false;
  }
  if (desc.Format != D3DFMT_A8R8G8B8 && desc.Format != D3DFMT_X8R8G8B8) {
    snprintf(why, 64, "volume format %u pool %u", unsigned(desc.Format), unsigned(desc.Pool));
    return false;
  }
  D3DLOCKED_BOX locked = {};
  if (const HRESULT result = texture->LockBox(0, &locked, nullptr, D3DLOCK_READONLY); FAILED(result) || !locked.pBits) {
    snprintf(why, 64, "volume lock %08lx pool %u", static_cast<unsigned long>(result), unsigned(desc.Pool));
    return false;
  }
  width = desc.Width;
  height = desc.Height;
  depth = desc.Depth;
  pixels.resize(size_t(width) * height * depth * 4);
  const auto *source = static_cast<const uint8_t *>(locked.pBits);
  for (uint32_t z = 0; z < depth; ++z)
    for (uint32_t y = 0; y < height; ++y)
      std::memcpy(pixels.data() + (size_t(z) * height + y) * width * 4,
                  source + size_t(z) * locked.SlicePitch + size_t(y) * locked.RowPitch, size_t(width) * 4);
  if (desc.Format == D3DFMT_X8R8G8B8)
    for (size_t i = 3; i < pixels.size(); i += 4)
      pixels[i] = 255;
  texture->UnlockBox(0);
  return true;
}

bool depthShaders() {
  if (g_depthShadersFailed)
    return false;
  if (g_depthVertex && g_depthFragment)
    return true;
  const remixapi_HostShaderBindingEXT sampler = {0, REMIXAPI_HOST_BINDING_SAMPLER_EXT};
  remixapi_HostShaderInfoEXT info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_HOST_SHADER_INFO_EXT;
  info.pushConstantBytes = 8;
  info.stage = REMIXAPI_HOST_SHADER_VERTEX_EXT;
  info.code = host_depth_vert;
  info.codeBytes = sizeof(host_depth_vert);
  if (g_registerShader(&info, &g_depthVertex) != REMIXAPI_ERROR_CODE_SUCCESS) {
    g_depthShadersFailed = true;
    return false;
  }
  info.stage = REMIXAPI_HOST_SHADER_FRAGMENT_EXT;
  info.code = host_depth_frag;
  info.codeBytes = sizeof(host_depth_frag);
  info.bindings = &sampler;
  info.bindingCount = 1;
  if (g_registerShader(&info, &g_depthFragment) != REMIXAPI_ERROR_CODE_SUCCESS) {
    g_depthShadersFailed = true;
    return false;
  }
  info.code = host_frame_depth_frag;
  info.codeBytes = sizeof(host_frame_depth_frag);
  if (g_registerShader(&info, &g_frameDepthFragment) != REMIXAPI_ERROR_CODE_SUCCESS) {
    g_depthShadersFailed = true;
    return false;
  }
  return true;
}


const Shader *shaderFor(const void *object) {
  const auto code = proxy::d3d::shaderCode(object);
  if (!code || code->empty())
    return nullptr;
  const uint64_t hash = proxy::dxvk::textureHash(reinterpret_cast<const uint8_t *>(code->data()), code->size() * 4);
  auto found = g_shaders.find(hash);
  if (found != g_shaders.end())
    return found->second.unsupported ? nullptr : &found->second;
  Shader &shader = g_shaders[hash];
  shader.hash = hash;
  try {
    ::dxvk::DxsoReader reader(reinterpret_cast<const char *>(code->data()));
    ::dxvk::DxsoModule module(reader);
    ::dxvk::DxsoModuleInfo info = {};
    const auto analysis = module.analyze();
    shader.vertex = module.info().type() == ::dxvk::DxsoProgramTypes::VertexShader;
    ::dxvk::D3D9ConstantLayout layout = {};
    layout.floatCount = shader.vertex ? ::dxvk::caps::MaxFloatConstantsVS : ::dxvk::caps::MaxFloatConstantsPS;
    layout.intCount = ::dxvk::caps::MaxOtherConstants;
    layout.boolCount = ::dxvk::caps::MaxOtherConstants;
    layout.bitmaskCount = 1;
    char name[32];
    snprintf(name, sizeof(name), "%s_%016llx", shader.vertex ? "vs" : "ps", (unsigned long long)hash);
    const auto compiled = module.compile(info, name, analysis, layout);
    for (const auto &b : compiled->bindings()) {
      if (b.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
        shader.bindings.push_back({b.resourceBinding, REMIXAPI_HOST_BINDING_UNIFORM_BUFFER_EXT});
      else if (b.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
        shader.bindings.push_back({b.resourceBinding, REMIXAPI_HOST_BINDING_SAMPLER_EXT});
      else
        shader.unsupported = true;
    }
    shader.inputMask = compiled->info().inputMask;
    const auto &isgn = module.isgn();
    for (uint32_t i = 0; i < isgn.elemCount; ++i)
      shader.inputs.push_back({uint32_t(isgn.elems[i].semantic.usage), isgn.elems[i].semantic.usageIndex,
                               isgn.elems[i].slot});
    const auto &spirv = compiled->code();
    remixapi_HostShaderInfoEXT registration = {};
    registration.sType = REMIXAPI_STRUCT_TYPE_HOST_SHADER_INFO_EXT;
    registration.stage = shader.vertex ? REMIXAPI_HOST_SHADER_VERTEX_EXT : REMIXAPI_HOST_SHADER_FRAGMENT_EXT;
    registration.code = spirv.data();
    registration.codeBytes = spirv.size();
    registration.bindings = shader.bindings.data();
    registration.bindingCount = uint32_t(shader.bindings.size());
    registration.pushConstantBytes = compiled->info().pushConstSize;
    if (!shader.unsupported && g_registerShader(&registration, &shader.handle) != REMIXAPI_ERROR_CODE_SUCCESS)
      shader.unsupported = true;
  } catch (...) {
    shader.unsupported = true;
  }
  shader.unsupported ? ++g_stats.failed : ++g_stats.compiled;
  return shader.unsupported ? nullptr : &shader;
}

// A recorded pass with its data.
struct Pass {
  remixapi_HostPassEXT info = {};
  std::vector<std::vector<uint8_t>> buffers; // by binding order in `bindings`
  std::vector<remixapi_HostPassBufferEXT> bindings;
  std::vector<remixapi_HostPassSamplerEXT> samplers;
  std::vector<std::vector<uint8_t>> vertexData;
  std::vector<remixapi_HostPassVertexBindingEXT> vertexBindings;
  std::vector<remixapi_HostPassAttributeEXT> attributes;
  std::vector<uint32_t> indices;
  std::vector<uint8_t> push; // the push constants (dxso: D3D9RenderStateInfo)
  std::string name; // material, for the dump
  float projection[2] = {}; // p22, p32 of a depth-tested draw on the frame (its depth test's terms)
  uint64_t vsHash = 0, psHash = 0;
  uint32_t inputMask = 0;
};
std::vector<Pass> g_passes;

// A full-screen pass of the proxy's own shaders (the view-depth passes).
Pass fullScreen(remixapi_HostShaderHandleEXT fragment, const float push[2], const char *name) {
  Pass pass;
  auto &info = pass.info;
  info.kind = REMIXAPI_HOST_PASS_DRAW_EXT;
  info.vertexShader = g_depthVertex;
  info.fragmentShader = fragment;
  const float triangle[6] = {-1, -1, 3, -1, -1, 3};
  pass.vertexData.emplace_back(reinterpret_cast<const uint8_t *>(triangle),
                               reinterpret_cast<const uint8_t *>(triangle) + sizeof(triangle));
  pass.vertexBindings.push_back({nullptr, sizeof(triangle), 8});
  pass.attributes.push_back({0, 0, kFormatR32G32Sfloat, 0});
  pass.indices = {0, 1, 2};
  pass.samplers.push_back({0, REMIXAPI_HOST_TARGET_VIEW_DEPTH_EXT, 0, 1, 1});
  pass.push.assign(reinterpret_cast<const uint8_t *>(push), reinterpret_cast<const uint8_t *>(push) + 8);
  info.srcColorBlend = info.srcAlphaBlend = 1; // ONE (no blending)
  info.colorWriteMask = 0xf;
  pass.name = name;
  return pass;
}
uint64_t g_frame = 0;
// Targets a draw sampled or a copy read, by the frame it last happened:
// copies into and clears of other targets (reflection views, the engine's
// own copies the path tracer makes moot) are not replayed.
std::unordered_map<const void *, uint64_t> g_targetUse;
constexpr uint64_t kUseFrames = 120;
// Targets some submitted pass wrote: a draw sampling any other (one cleared
// once at startup, before anything sampled it, say) gets it cleared first,
// to its last clear colour, or the runtime has no image to sample.
std::unordered_set<const void *> g_established;
FILE *g_dump = nullptr; // the flat dump while it is active (set at submit, for the next frame's records)

bool fill(remixapi_HostPassEXT &info, const void *target) {
  info.target = reinterpret_cast<uint64_t>(target);
  if (!target)
    return true;
  uint32_t width = 0, height = 0, format = 0;
  if (!proxy::d3d::targetTexture(target, width, height, format))
    return false;
  info.targetWidth = width;
  info.targetHeight = height;
  info.targetFormat = targetFormat(format);
  info.targetChannels = targetChannels(format);
  return true;
}

void used(const void *target) {
  if (target)
    g_targetUse[target] = g_frame;
}
} // namespace

// The extension's entry points, when the runtime has them (and
// OPENREMIX_PROXY_HOST_PASSES is not 0).
static bool available() {
  static const bool initialized = [] {
    char value[8] = {};
    if (GetEnvironmentVariableA("OPENREMIX_PROXY_HOST_PASSES", value, sizeof(value)) && value[0] == '0')
      return false;
    const HMODULE runtime = GetModuleHandleW(L"remix.dll");
    if (!runtime)
      return false;
    g_registerShader = reinterpret_cast<PFN_remixapi_RegisterHostShaderEXT>(
        reinterpret_cast<void *>(GetProcAddress(runtime, "remixapi_RegisterHostShaderEXT")));
    g_submit = reinterpret_cast<PFN_remixapi_SubmitHostPassesEXT>(
        reinterpret_cast<void *>(GetProcAddress(runtime, "remixapi_SubmitHostPassesEXT")));
    g_statistics = reinterpret_cast<PFN_remixapi_GetHostPassStatisticsEXT>(
        reinterpret_cast<void *>(GetProcAddress(runtime, "remixapi_GetHostPassStatisticsEXT")));
    g_updateTarget = reinterpret_cast<PFN_remixapi_UpdateHostTargetEXT>(
        reinterpret_cast<void *>(GetProcAddress(runtime, "remixapi_UpdateHostTargetEXT")));
    g_updateVolume = reinterpret_cast<PFN_remixapi_UpdateHostVolumeEXT>(
        reinterpret_cast<void *>(GetProcAddress(runtime, "remixapi_UpdateHostVolumeEXT")));
    g_passesEnabled = reinterpret_cast<PFN_remixapi_GetHostPassesEnabledEXT>(
        reinterpret_cast<void *>(GetProcAddress(runtime, "remixapi_GetHostPassesEnabledEXT")));
    // Without the setting's getter (an older runtime) there is no way to turn
    // them on: the proxy behaves as without host passes.
    return g_registerShader && g_submit && g_updateTarget && g_passesEnabled;
  }();
  return initialized;
}

// The user's "Pass-through game shaders (experimental)" setting
// (rtx.native.hostPasses), read once a frame at submit: off, nothing is
// recorded and every draw takes the path it took before host passes.
static void poll() {
  remixapi_Bool on = 0;
  if (!available() || g_passesEnabled(&on) != REMIXAPI_ERROR_CODE_SUCCESS)
    on = 0;
  if (on && !g_enabled) {
    // What the runtime holds may be stale or gone: start afresh.
    g_established.clear();
    g_uploaded.clear();
    g_depthDone.clear();
    g_targetUse.clear();
  }
  g_enabled = on != 0;
}

bool enabled() { return g_enabled; }

bool wants(const remix::Draw &d) {
  return enabled() && (d.target || d.textureTargets) && d.pixelShader && d.vertexShader && (d.colorWrite & 0xf);
}

// Source's own post-processing: its bloom (downsample, blur, add) and its
// HDR passes (tonemap, luminance, float-to-screen, engine_post), all dev/
// materials (GMod's Lua effects use pp/ ones). The runtime's post-processing
// (exposure, bloom, tonemapping) does that job on the path-traced frame, so
// these are dropped: neither replayed nor drawn. OPENREMIX_PROXY_ENGINE_POST=1
// keeps them.
bool enginePost(const char *name) {
  static const bool keep = [] {
    char value[8] = {};
    return GetEnvironmentVariableA("OPENREMIX_PROXY_ENGINE_POST", value, sizeof(value)) && value[0] == '1';
  }();
  if (keep || _strnicmp(name, "dev/", 4) != 0)
    return false;
  for (const char *prefix : {"downsample", "blurfilter", "bloom", "engine_post", "lumcompare", "floattoscreen",
                             "tonemap", "hdr"})
    if (_strnicmp(name + 4, prefix, std::strlen(prefix)) == 0)
      return true;
  return false;
}

bool recordDraw(const remix::Draw &d, const float projection[16]) {
  if (!enabled())
    return false;
  if (enginePost(engine_materials::currentName())) {
    ++g_stats.enginePost;
    return true; // handled: nothing to replay or draw
  }
  const Shader *vs = shaderFor(d.vertexShader), *ps = shaderFor(d.pixelShader);
  if (!vs || !ps || !vs->vertex || ps->vertex) {
    ++g_stats.noShader;
    return false;
  }
  Pass pass;
  auto &info = pass.info;
  info.kind = REMIXAPI_HOST_PASS_DRAW_EXT;
  if (!fill(info, d.target)) {
    ++g_stats.noData;
    return false;
  }
  info.vertexShader = vs->handle;
  info.fragmentShader = ps->handle;
  // Geometry: the vertices the draw indexes, rebased, as 32-bit indices.
  uint32_t indexCount = 0;
  if (d.type == D3DPT_TRIANGLELIST) {
    info.topology = 0;
    indexCount = d.primCount * 3;
  } else if (d.type == D3DPT_TRIANGLESTRIP) {
    info.topology = 1;
    indexCount = d.primCount + 2;
  } else {
    ++g_stats.badPrimitive;
    return false;
  }
  const uint32_t indexBytes = d.indices.is32 ? 4 : 2;
  if (!d.indices.data || uint64_t(d.startIndex + indexCount) * indexBytes > d.indices.size || !d.numVertices ||
      indexCount > (1u << 20)) {
    ++g_stats.noData;
    return false;
  }
  pass.indices.resize(indexCount);
  for (uint32_t i = 0; i < indexCount; ++i) {
    uint32_t index = 0;
    if (d.indices.is32)
      std::memcpy(&index, d.indices.data + (d.startIndex + i) * 4, 4);
    else {
      uint16_t narrow = 0;
      std::memcpy(&narrow, d.indices.data + (d.startIndex + i) * 2, 2);
      index = narrow;
    }
    pass.indices[i] = index >= d.minIndex && index - d.minIndex < d.numVertices ? index - d.minIndex : 0;
  }
  int bindingOf[4] = {-1, -1, -1, -1};
  for (const D3DVERTEXELEMENT9 *e = d.elements; e && e->Stream != 0xFF; ++e) {
    const Shader::Input *input = nullptr;
    for (const auto &candidate : vs->inputs)
      if (candidate.usage == e->Usage && candidate.usageIndex == e->UsageIndex)
        input = &candidate;
    const uint32_t format = vertexFormat(e->Type);
    if (!input || e->Stream >= 4 || !(vs->inputMask & (1u << input->location)) || !format)
      continue;
    const uint32_t location = input->location;
    if (bindingOf[e->Stream] < 0) {
      const auto &s = d.streams[e->Stream];
      const uint64_t first = uint64_t(s.offset) + uint64_t(int64_t(d.baseVertex) + d.minIndex) * s.stride;
      const uint64_t bytes = uint64_t(d.numVertices) * s.stride;
      if (!s.data || !s.stride || first + bytes > s.size || bytes > (16u << 20)) {
        ++g_stats.noData;
        return false;
      }
      bindingOf[e->Stream] = int(pass.vertexData.size());
      pass.vertexData.emplace_back(s.data + first, s.data + first + bytes);
      pass.vertexBindings.push_back({nullptr, uint32_t(bytes), s.stride});
    }
    pass.attributes.push_back({location, uint32_t(bindingOf[e->Stream]), format, e->Offset});
  }
  if (pass.attributes.empty()) {
    ++g_stats.noData;
    return false;
  }
  // Samplers: the frame's copies and the host's targets only (a plain texture
  // has no image on the runtime's side).
  ::dxvk::D3D9SpecializationInfo spec;
  uint32_t samplerTypes = 0, samplerNull = 0;
  for (const auto *shader : {vs, ps})
    for (const auto &b : shader->bindings) {
      if (b.type != REMIXAPI_HOST_BINDING_SAMPLER_EXT)
        continue;
      const bool pixel = !shader->vertex;
      const uint32_t n = pixel ? b.binding - ::dxvk::computeResourceSlotId(::dxvk::DxsoProgramTypes::PixelShader,
                                                                         ::dxvk::DxsoBindingType::Image, 0)
                               : b.binding - ::dxvk::computeResourceSlotId(::dxvk::DxsoProgramTypes::VertexShader,
                                                                         ::dxvk::DxsoBindingType::Image, 0);
      remixapi_HostPassSamplerEXT sampler = {};
      sampler.binding = b.binding;
      const void *texture = pixel && n < 16 ? d.textures[n] : nullptr;
      if (!texture) {
        samplerNull |= 1u << (pixel ? n : 16 + n); // the shader reads nothing from it
        sampler.target = 0;
        sampler.linear = 1;
      } else {
        const auto type = const_cast<IDirect3DBaseTexture9 *>(static_cast<const IDirect3DBaseTexture9 *>(texture))->GetType();
        if (d.textureTargets & (1u << n)) {
          used(texture);
        } else {
          // A plain texture: its contents go up as a host target, once per
          // version of them.
          uint32_t width = 0, height = 0, version = 0;
          bool bgra = false;
          if (type == D3DRTYPE_VOLUMETEXTURE) {
            // A volume: read through D3D once, up as a sampled-only target.
            auto &uploaded = g_uploaded[texture];
            if (uploaded.version != 1) {
              std::vector<uint8_t> pixels;
              uint32_t depth = 0;
              char why[64] = "no volume support in the runtime";
              remixapi_ErrorCode result = REMIXAPI_ERROR_CODE_SUCCESS;
              if (!g_updateVolume || !lockVolume(texture, width, height, depth, pixels, why) ||
                  (result = g_updateVolume(reinterpret_cast<uint64_t>(texture), width, height, depth,
                                           kFormatB8G8R8A8Unorm, pixels.data(), pixels.size())) !=
                      REMIXAPI_ERROR_CODE_SUCCESS) {
                if (result != REMIXAPI_ERROR_CODE_SUCCESS)
                  snprintf(why, sizeof(why), "runtime refused the %ux%ux%u volume: %d", width, height, depth,
                           int(result));
                ++g_stats.plainTexture;
                if (g_dump)
                  fprintf(g_dump, "host skip %s: sampler %u volume texture %p (%s)\n", engine_materials::currentName(),
                          n, texture, why);
                return false;
              }
              uploaded = {1, width, height};
              g_established.insert(texture);
              ++g_stats.uploads;
            }
          } else if (type != D3DRTYPE_TEXTURE) {
            ++g_stats.plainTexture;
            if (g_dump)
              fprintf(g_dump, "host skip %s: sampler %u plain texture %p type %d (not 2D)\n",
                      engine_materials::currentName(), n, texture, int(type));
            return false;
          } else {
          // Without a CPU copy (a large compressed texture), the texture is
          // read through D3D once (version 1: contents taken as static).
          const bool copied = remix::texturePixels(texture, width, height, version, bgra, nullptr);
          if (!copied)
            version = 1;
          auto &uploaded = g_uploaded[texture];
          if (uploaded.version != version || (copied && (uploaded.width != width || uploaded.height != height))) {
            std::vector<uint8_t> pixels;
            char why[64] = "unsupported copy";
            if (!(copied ? remix::texturePixels(texture, width, height, version, bgra, &pixels)
                         : lockPixels(texture, width, height, bgra, pixels, why)) ||
                g_updateTarget(reinterpret_cast<uint64_t>(texture), width, height,
                               bgra ? kFormatB8G8R8A8Unorm : kFormatR8G8B8A8Unorm, pixels.data(),
                               pixels.size()) != REMIXAPI_ERROR_CODE_SUCCESS) {
              ++g_stats.plainTexture;
              if (g_dump)
                fprintf(g_dump, "host skip %s: sampler %u plain texture %p (%s)\n", engine_materials::currentName(), n,
                        texture, why);
              return false;
            }
            uploaded = {version, width, height};
            g_established.insert(texture);
            ++g_stats.uploads;
          }
          }
        }
        sampler.target = reinterpret_cast<uint64_t>(texture);
        const auto *states = d.samplerStates ? d.samplerStates[n] : nullptr;
        const uint32_t mag = states ? states[D3DSAMP_MAGFILTER] : D3DTEXF_LINEAR;
        sampler.linear = mag != D3DTEXF_POINT && mag != D3DTEXF_NONE;
        auto address = [](DWORD mode) -> uint32_t {
          return mode == D3DTADDRESS_WRAP ? 0 : (mode == D3DTADDRESS_MIRROR || mode == D3DTADDRESS_MIRRORONCE) ? 2 : 1;
        };
        sampler.addressU = states ? address(states[D3DSAMP_ADDRESSU]) : 1;
        sampler.addressV = states ? address(states[D3DSAMP_ADDRESSV]) : 1;
        if (pixel)
          // DxsoSamplerType (dxso_compiler.h): 2D 0, 3D 1, cube 2.
          samplerTypes |= (type == D3DRTYPE_CUBETEXTURE ? 2u : type == D3DRTYPE_VOLUMETEXTURE ? 1u : 0u) << (2 * n);
      }
      pass.samplers.push_back(sampler);
    }
  spec.set<::dxvk::SpecSamplerType>(samplerTypes);
  spec.set<::dxvk::SpecSamplerNull>(samplerNull);
  spec.set<::dxvk::SpecAlphaCompareOp>(d.alphaTest && d.alphaFunc >= D3DCMP_NEVER && d.alphaFunc <= D3DCMP_ALWAYS
                                         ? d.alphaFunc - 1
                                         : uint32_t(VK_COMPARE_OP_ALWAYS));
  spec.set<::dxvk::SpecAlphaPrecisionBits>(8u);
  spec.set<::dxvk::SpecVertexShaderBools>(d.vsBools & 0xffff);
  spec.set<::dxvk::SpecPixelShaderBools>(d.psBools & 0xffff);
  // Constants: {int4 i[16]; float4 f[N]} for each stage, the shared pixel
  // state (bump environment stages: none) and the specialization block.
  for (const auto *shader : {vs, ps})
    for (const auto &b : shader->bindings) {
      if (b.type != REMIXAPI_HOST_BINDING_UNIFORM_BUFFER_EXT)
        continue;
      std::vector<uint8_t> data;
      if (b.binding == ::dxvk::computeResourceSlotId(::dxvk::DxsoProgramTypes::VertexShader,
                                                    ::dxvk::DxsoBindingType::ConstantBuffer, ::dxvk::VSConstantBuffer)) {
        data.resize(16 * 16 + 256 * 16);
        if (d.vsConstantsI)
          std::memcpy(data.data(), d.vsConstantsI, 16 * 16);
        if (d.vsConstants)
          std::memcpy(data.data() + 16 * 16, d.vsConstants, 256 * 16);
      } else if (b.binding == ::dxvk::computeResourceSlotId(::dxvk::DxsoProgramTypes::PixelShader,
                                                           ::dxvk::DxsoBindingType::ConstantBuffer,
                                                           ::dxvk::PSConstantBuffer)) {
        data.resize(16 * 16 + 224 * 16);
        if (d.psConstantsI)
          std::memcpy(data.data(), d.psConstantsI, 16 * 16);
        if (d.psConstants)
          std::memcpy(data.data() + 16 * 16, d.psConstants, 224 * 16);
      } else if (b.binding == ::dxvk::getSpecConstantBufferSlot()) {
        data.resize(::dxvk::D3D9SpecializationInfo::MaxSpecDwords * 4);
        std::memcpy(data.data(), spec.data.data(), data.size());
      } else {
        data.resize(512); // PSShared and anything else: zeros
      }
      pass.bindings.push_back({b.binding, nullptr, uint32_t(data.size())});
      pass.buffers.push_back(std::move(data));
    }
  ::dxvk::D3D9RenderStateInfo renderState = {};
  renderState.alphaRef = d.alphaRef & 0xff;
  pass.push.assign(reinterpret_cast<const uint8_t *>(&renderState),
                   reinterpret_cast<const uint8_t *>(&renderState) + sizeof(renderState));
  // Blending, as the draw has it.
  info.blendEnable = d.blend;
  info.srcColorBlend = blendFactor(d.srcBlend);
  info.dstColorBlend = blendFactor(d.dstBlend);
  info.colorBlendOp = blendOp(d.blendOp);
  info.srcAlphaBlend = blendFactor(d.separateAlpha ? d.srcBlendAlpha : d.srcBlend);
  info.dstAlphaBlend = blendFactor(d.separateAlpha ? d.dstBlendAlpha : d.dstBlend);
  info.alphaBlendOp = blendOp(d.separateAlpha ? d.blendOpAlpha : d.blendOp);
  info.colorWriteMask = d.colorWrite & 0xf;
  // A depth test on the frame: against the runtime's depth (see submit).
  // The test only, without the draw's depth write: a volumetric sky shader
  // writing depth lost the device (NVIDIA), and nothing after a host draw
  // reads the depth it would have written. OPENREMIX_PROXY_HOST_DEPTH: 0 no
  // depth tests, 1 tests and writes, 2 tests only (default).
  static const int depthMode = [] {
    char value[8] = {};
    return GetEnvironmentVariableA("OPENREMIX_PROXY_HOST_DEPTH", value, sizeof(value)) ? atoi(value) : 2;
  }();
  // Flat draws (screen-space quads) never test: the engine's 2D projection
  // puts them in front of the scene.
  const bool perspective = std::fabs(std::fabs(projection[11]) - 1.0f) < 1e-3f && std::fabs(projection[15]) < 1e-3f;
  if (depthMode && perspective && !d.target && d.depthTest && d.zFunc >= D3DCMP_NEVER && d.zFunc <= D3DCMP_ALWAYS) {
    info.depth = d.zFunc | (d.zWrite ? REMIXAPI_HOST_DEPTH_WRITE_EXT : 0u); // D3DCMP == VkCompareOp + 1
    if (depthMode != 1)
      info.depth &= 0xff;
    // Device z = (m10 z + m14) / (m11 z) (D3D row vectors); for the view depth
    // vz = |z|: with m11 = -1 (Source looks down -z) m10's sign flips.
    pass.projection[0] = projection[11] < 0 ? -projection[10] : projection[10];
    pass.projection[1] = projection[14];
  }
  info.viewport[0] = int32_t(d.viewport.X);
  info.viewport[1] = int32_t(d.viewport.Y);
  info.viewport[2] = int32_t(d.viewport.Width);
  info.viewport[3] = int32_t(d.viewport.Height);
  if (d.scissorTest) {
    info.scissor[0] = d.scissor.left;
    info.scissor[1] = d.scissor.top;
    info.scissor[2] = d.scissor.right - d.scissor.left;
    info.scissor[3] = d.scissor.bottom - d.scissor.top;
  }
  pass.name = engine_materials::currentName();
  pass.vsHash = vs->hash;
  pass.psHash = ps->hash;
  pass.inputMask = vs->inputMask;
  g_passes.push_back(std::move(pass));
  return true;
}

bool recordDepth(const remix::Draw &d) {
  if (!enabled() || !d.target || !d.psConstants || !depthShaders())
    return false;
  // DepthWrite's g_vNearFarPlanes (c1): red = view depth / far plane.
  const float farPlane = d.psConstants[1 * 4 + 1];
  if (!(farPlane > 1))
    return false;
  if (auto done = g_depthDone.find(d.target); done != g_depthDone.end() && done->second == g_frame)
    return true; // this frame's pass is in the list already: the draw is moot
  float push[2] = {1.f / farPlane, 1.f};
  if (auto clear = g_clearColor.find(d.target); clear != g_clearColor.end())
    push[1] = ((clear->second >> 16) & 0xff) / 255.f;
  Pass pass = fullScreen(g_depthFragment, push, "(view depth)");
  if (!fill(pass.info, d.target)) {
    ++g_stats.noData;
    return false;
  }
  g_passes.push_back(std::move(pass));
  g_depthDone[d.target] = g_frame;
  ++g_stats.depth;
  return true;
}

void recordCopy(const void *source, const RECT *sourceRect, const void *target, const RECT *targetRect, bool linear) {
  if (!enabled())
    return;
  Pass pass;
  auto &info = pass.info;
  info.kind = REMIXAPI_HOST_PASS_COPY_EXT;
  if (!fill(info, target))
    return;
  info.source = reinterpret_cast<uint64_t>(source);
  used(source);
  auto rect = [](const RECT *r, int32_t out[4]) {
    if (r) {
      out[0] = r->left;
      out[1] = r->top;
      out[2] = r->right - r->left;
      out[3] = r->bottom - r->top;
    }
  };
  rect(sourceRect, info.sourceRect);
  rect(targetRect, info.targetRect);
  info.linear = linear;
  g_passes.push_back(std::move(pass));
}

void recordClear(const void *target, uint32_t argb) {
  if (!enabled() || !target)
    return;
  Pass pass;
  auto &info = pass.info;
  info.kind = REMIXAPI_HOST_PASS_CLEAR_EXT;
  if (!fill(info, target))
    return;
  info.clearColor[0] = ((argb >> 16) & 0xff) / 255.f;
  info.clearColor[1] = ((argb >> 8) & 0xff) / 255.f;
  info.clearColor[2] = (argb & 0xff) / 255.f;
  info.clearColor[3] = (argb >> 24) / 255.f;
  g_clearColor[target] = argb;
  g_passes.push_back(std::move(pass));
}

void submit(FILE *dump) {
  poll();
  if (!enabled()) {
    g_passes.clear(); // recorded before the setting went off
    return;
  }
  ++g_frame;
  g_dump = dump;
  // A frame the runtime did not replay (loading) dropped its passes: what
  // they wrote (a clear of a target made once at startup) has to be redone.
  static bool sentLast = false;
  if (sentLast && g_statistics) {
    remixapi_HostPassStatisticsEXT ran = {};
    ran.structSize = sizeof(ran);
    if (g_statistics(&ran) == REMIXAPI_ERROR_CODE_SUCCESS && ran.passes == 0) {
      g_established.clear();
      g_uploaded.clear();
      ++g_stats.dropped;
    }
  }
  // Copies into and clears of targets nothing draws from are dropped; the
  // frame itself is always a live target.
  std::vector<remixapi_HostPassEXT> list;
  list.reserve(g_passes.size() + 4);
  std::vector<Pass> fills; // the frame-depth fill pass, whose data `list` points into
  fills.reserve(2);
  bool frameDepthFilled = false;
  for (auto &pass : g_passes) {
    auto &info = pass.info;
    if (info.kind != REMIXAPI_HOST_PASS_DRAW_EXT && info.target) {
      auto use = g_targetUse.find(reinterpret_cast<const void *>(info.target));
      if (use == g_targetUse.end() || use->second + kUseFrames < g_frame) {
        ++g_stats.pruned;
        continue;
      }
    }
    if (info.kind == REMIXAPI_HOST_PASS_DRAW_EXT)
      for (const auto &sm : pass.samplers) {
        const void *sampled = reinterpret_cast<const void *>(sm.target);
        if (!sm.target || sm.target >= REMIXAPI_HOST_TARGET_RESERVED_EXT || g_established.count(sampled))
          continue;
        remixapi_HostPassEXT clear = {};
        clear.kind = REMIXAPI_HOST_PASS_CLEAR_EXT;
        if (!fill(clear, sampled))
          continue;
        if (auto color = g_clearColor.find(sampled); color != g_clearColor.end()) {
          clear.clearColor[0] = ((color->second >> 16) & 0xff) / 255.f;
          clear.clearColor[1] = ((color->second >> 8) & 0xff) / 255.f;
          clear.clearColor[2] = (color->second & 0xff) / 255.f;
          clear.clearColor[3] = (color->second >> 24) / 255.f;
        }
        list.push_back(clear);
        g_established.insert(sampled);
        ++g_stats.clears;
        if (dump)
          fprintf(dump, "host clear %llx (first use)\n", (unsigned long long)clear.target);
      }
    if (info.target && info.target < REMIXAPI_HOST_TARGET_RESERVED_EXT)
      g_established.insert(reinterpret_cast<const void *>(info.target));
    if (info.kind == REMIXAPI_HOST_PASS_DRAW_EXT && info.depth && !frameDepthFilled) {
      // The frame's depth from the runtime's, in this draw's projection.
      if (!depthShaders()) {
        info.depth = 0; // drawn without the test
      } else {
        Pass fillPass = fullScreen(g_frameDepthFragment, pass.projection, "(frame depth)");
        fillPass.info.target = REMIXAPI_HOST_TARGET_FRAME_DEPTH_EXT;
        fillPass.info.depth = (uint32_t(VK_COMPARE_OP_ALWAYS) + 1) | REMIXAPI_HOST_DEPTH_WRITE_EXT;
        fillPass.info.colorWriteMask = 0;
        for (size_t i = 0; i < fillPass.bindings.size(); ++i)
          fillPass.bindings[i].data = fillPass.buffers[i].data();
        fillPass.vertexBindings[0].data = fillPass.vertexData[0].data();
        auto &fi = fillPass.info;
        fi.samplers = fillPass.samplers.data();
        fi.samplerCount = 1;
        fi.pushConstants = fillPass.push.data();
        fi.pushConstantBytes = uint32_t(fillPass.push.size());
        fi.vertexBindings = fillPass.vertexBindings.data();
        fi.vertexBindingCount = 1;
        fi.attributes = fillPass.attributes.data();
        fi.attributeCount = 1;
        fi.indices = fillPass.indices.data();
        fi.indexCount = 3;
        list.push_back(fi);
        fills.push_back(std::move(fillPass)); // keeps the data alive until submitted
        frameDepthFilled = true;
        ++g_stats.draws;
        if (dump)
          fprintf(dump, "host draw (frame depth) p22=%g p32=%g\n", pass.projection[0], pass.projection[1]);
      }
    }
    if (info.kind == REMIXAPI_HOST_PASS_DRAW_EXT) {
      for (size_t i = 0; i < pass.bindings.size(); ++i)
        pass.bindings[i].data = pass.buffers[i].data();
      for (size_t i = 0; i < pass.vertexBindings.size(); ++i)
        pass.vertexBindings[i].data = pass.vertexData[i].data();
      info.buffers = pass.bindings.data();
      info.bufferCount = uint32_t(pass.bindings.size());
      info.samplers = pass.samplers.data();
      info.samplerCount = uint32_t(pass.samplers.size());
      info.pushConstants = pass.push.data();
      info.pushConstantBytes = uint32_t(pass.push.size());
      info.vertexBindings = pass.vertexBindings.data();
      info.vertexBindingCount = uint32_t(pass.vertexBindings.size());
      info.attributes = pass.attributes.data();
      info.attributeCount = uint32_t(pass.attributes.size());
      info.indices = pass.indices.data();
      info.indexCount = uint32_t(pass.indices.size());
      ++g_stats.draws; // sent
    } else if (info.kind == REMIXAPI_HOST_PASS_COPY_EXT)
      ++g_stats.copies;
    else
      ++g_stats.clears;
    if (dump) {
      if (info.kind == REMIXAPI_HOST_PASS_DRAW_EXT) {
        fprintf(dump, "host draw %s target=%llx %ux%u vs=%016llx ps=%016llx samplers=%u blend=%d %u/%u depth=%x verts=%u idx=%u "
                      "viewport=%d,%d %dx%d inputs=%x attributes:",
                pass.name.c_str(), (unsigned long long)info.target, info.targetWidth, info.targetHeight,
                (unsigned long long)pass.vsHash, (unsigned long long)pass.psHash, info.samplerCount,
                int(info.blendEnable), info.srcColorBlend, info.dstColorBlend, info.depth,
                info.vertexBindingCount ? info.vertexBindings[0].bytes / info.vertexBindings[0].stride : 0,
                info.indexCount, info.viewport[0], info.viewport[1], info.viewport[2], info.viewport[3], pass.inputMask);
        for (const auto &a : pass.attributes)
          fprintf(dump, " %u:b%u/f%u@%u", a.location, a.binding, a.format, a.offset);
        fprintf(dump, " bindings:");
        for (const auto &b : pass.bindings)
          fprintf(dump, " %u(%u)", b.binding, b.bytes);
        for (const auto &sm : pass.samplers)
          fprintf(dump, " s%u=%llx", sm.binding, (unsigned long long)sm.target);
        if (!pass.vertexData.empty() && pass.vertexBindings[0].stride >= 12) {
          const uint32_t stride = pass.vertexBindings[0].stride;
          fprintf(dump, " v0..3:");
          for (uint32_t v = 0; v < 4 && (v + 1) * stride <= pass.vertexData[0].size(); ++v) {
            float f[5] = {};
            std::memcpy(f, pass.vertexData[0].data() + v * stride, std::min<uint32_t>(20, stride));
            fprintf(dump, " (%.3g %.3g %.3g %.3g %.3g)", f[0], f[1], f[2], f[3], f[4]);
          }
        }
        fprintf(dump, " idx:");
        for (uint32_t i = 0; i < std::min<uint32_t>(6, info.indexCount); ++i)
          fprintf(dump, " %u", info.indices[i]);
        // The pixel shader's first float constants (after its 16 int4s).
        for (size_t b = 0; b < pass.bindings.size(); ++b)
          if (pass.bindings[b].binding == 10 && pass.buffers[b].size() >= 256 + 16 * 16) {
            fprintf(dump, " ps c0..15:");
            for (uint32_t c = 0; c < 16; ++c) {
              float f[4];
              std::memcpy(f, pass.buffers[b].data() + 256 + c * 16, 16);
              fprintf(dump, " (%.3g %.3g %.3g %.3g)", f[0], f[1], f[2], f[3]);
            }
          }
        fprintf(dump, "\n");
      }
      else if (info.kind == REMIXAPI_HOST_PASS_COPY_EXT)
        fprintf(dump, "host copy %llx -> %llx %ux%u\n", (unsigned long long)info.source,
                (unsigned long long)info.target, info.targetWidth, info.targetHeight);
      else
        fprintf(dump, "host clear %llx\n", (unsigned long long)info.target);
    }
    list.push_back(info);
  }
  remixapi_HostPassListEXT submission = {};
  submission.sType = REMIXAPI_STRUCT_TYPE_HOST_PASS_LIST_EXT;
  submission.passes = list.data();
  submission.passCount = uint32_t(list.size());
  g_submit(&submission);
  sentLast = !list.empty();
  g_passes.clear();
  if (g_frame % 600 == 0) {
    for (auto it = g_targetUse.begin(); it != g_targetUse.end();)
      it = it->second + kUseFrames < g_frame ? g_targetUse.erase(it) : std::next(it);
    for (auto it = g_depthDone.begin(); it != g_depthDone.end();)
      it = it->second + kUseFrames < g_frame ? g_depthDone.erase(it) : std::next(it);
  }
}

void dumpStatistics(FILE *out) {
  fprintf(out,
          "== Host passes (%s): draws %u (view depth %u) copies %u clears %u pruned %u uploads %u frames dropped %u | "
          "Source bloom/HDR passes dropped %u | shaders compiled %u failed %u | skipped: no shader %u plain texture %u "
          "no data %u primitive %u\n",
          g_enabled ? "on" : available() ? "off: Pass-through game shaders setting" : "unavailable", g_stats.draws,
          g_stats.depth, g_stats.copies, g_stats.clears, g_stats.pruned, g_stats.uploads, g_stats.dropped, g_stats.enginePost, g_stats.compiled, g_stats.failed, g_stats.noShader, g_stats.plainTexture, g_stats.noData,
          g_stats.badPrimitive);
  remixapi_HostPassStatisticsEXT runtime = {};
  runtime.structSize = sizeof(runtime);
  if (g_statistics && g_statistics(&runtime) == REMIXAPI_ERROR_CODE_SUCCESS)
    fprintf(out,
            "   runtime last frame: passes %u draws %u copies %u clears %u skipped %u (copy source %u shaders %u pipeline %u "
            "descriptors %u buffer %u sampler %u target %u view depth %u) | shaders %u targets %u pipelines %u\n",
            runtime.passes, runtime.draws, runtime.copies, runtime.clears, runtime.skipped, runtime.skippedCopySource,
            runtime.skippedShaders, runtime.skippedPipeline, runtime.skippedDescriptors, runtime.skippedBuffer,
            runtime.skippedSampler, runtime.skippedTarget, runtime.skippedViewDepth, runtime.shaders, runtime.targets,
            runtime.pipelines);
  const uint32_t compiled = g_stats.compiled, failed = g_stats.failed;
  g_stats = {};
  g_stats.compiled = compiled;
  g_stats.failed = failed;
}
} // namespace proxy::hostpass
