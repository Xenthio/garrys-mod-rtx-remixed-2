// Standalone diagnostic host for the production BSP loader. It never injects
// into Source or writes installed content. Static VMTs only; no material proxies.
#include "bsp_world.h"
#include "replay_settings.h"
#include "replay_timing.h"
#include <remix/remix_bake_ext.h>
#include <remix/remix_native_ext.h>
#include <remix/remix_usd_ext.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <thread>

namespace {
using proxy::engine_materials::Shading;
using Clock = std::chrono::steady_clock;

void check(remixapi_ErrorCode result, const char *operation) {
  if (result != REMIXAPI_ERROR_CODE_SUCCESS)
    throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(unsigned(result)));
}

std::filesystem::file_time_type lastWrite(const std::filesystem::path &path) {
  std::error_code error;
  const auto time = std::filesystem::last_write_time(path, error);
  return error ? std::filesystem::file_time_type::min() : time;
}

// Materials and images outlive the retained BSP instances. Missing inputs stay
// unresolved and are reported once, rather than silently replaced for a bake.
class Materials {
public:
  explicit Materials(remixapi_Interface &api, bool reportHashes) : api(api), reportHashes(reportHashes) {}
  ~Materials() {
    for (auto &[key, material] : materials)
      api.DestroyMaterial(material);
    for (auto &[name, texture] : textures)
      if (texture.handle)
        api.DestroyTexture(texture.handle);
  }
  bool complete() const { return failures.empty(); }
  proxy::bsp::MaterialRef lookup(uint64_t sourceHash, const Shading *shading) {
    std::string base;
    if (!proxy::bsp::materialSource(sourceHash, base)) {
      fail("texture hash " + std::to_string(sourceHash), "no VMT base texture");
      return {};
    }
    // Include every static shading field: two VMTs may share their albedo.
    std::ostringstream key;
    key.precision(9);
    key << base << ':' << sourceHash;
    if (shading) {
      const auto &s = *shading;
      key << ':' << int(s.mode) << ':' << s.mask << ':' << s.maskScale << ':' << s.luminous;
      for (float c : s.tint)
        key << ':' << c;
      key << ':' << s.pbr.active << ':' << s.pbr.normal << ':' << s.pbr.ssbump << ':' << s.pbr.roughness
          << ':' << s.pbr.metallic;
      for (const auto &m : s.pbr.maps)
        key << ':' << m.texture << ':' << m.channel << ':' << m.curve << ':' << m.invert << ':' << m.exponent
            << ':' << m.bias << ':' << m.scale;
    }
    if (auto found = materials.find(key.str()); found != materials.end())
      return {found->second, {}};
    const auto albedo = image(base);
    if (albedo.empty())
      return {};
    std::wstring normal, maps[2], mask;
    if (shading) {
      normal = image(shading->pbr.normal);
      mask = image(shading->mask);
      for (int i = 0; i < 2; ++i)
        maps[i] = image(shading->pbr.maps[i].texture);
      if ((!shading->pbr.normal.empty() && normal.empty()) || (!shading->mask.empty() && mask.empty()))
        return {};
      for (int i = 0; i < 2; ++i)
        if (!shading->pbr.maps[i].texture.empty() && maps[i].empty())
          return {};
    }
    remixapi_MaterialInfoOpaqueEXT opaque = {};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.roughnessConstant = 1; // Match live plain-material exports.
    opaque.albedoConstant = {1, 1, 1};
    opaque.opacityConstant = 1;
    opaque.alphaTestType = 7;
    opaque.useDrawCallAlphaState = 1;
    // The decoded replay image has a temporary SDK texture ID. Material packs
    // still identify the original Source upload by its dxvk texture hash.
    remixapi_MaterialInfoReplacementHashEXT replacement{};
    replacement.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_REPLACEMENT_HASH_EXT;
    replacement.materialHash = proxy::bsp::textureReplacementHash(base, sourceHash);
    if (reportHashes)
      std::fprintf(stderr, "Material source %016llX, replacement %016llX: %s\n",
                   static_cast<unsigned long long>(sourceHash),
                   static_cast<unsigned long long>(replacement.materialHash), base.c_str());
    opaque.pNext = &replacement;
    remixapi_MaterialInfoEmissionEXT emission = {};
    emission.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_EMISSION_EXT;
    emission.pNext = &replacement;
    remixapi_MaterialInfoSelfIlluminationEXT self = {};
    self.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_SELF_ILLUMINATION_EXT;
    self.pNext = &emission;
    remixapi_MaterialInfoPbrEXT pbr = {};
    pbr.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_PBR_EXT;
    pbr.enabled = 1;
    remixapi_MaterialPbrRemapEXT remap = {};
    remap.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_PBR_REMAP_EXT;
    remixapi_MaterialNormalEncodingEXT encoding = {};
    encoding.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_NORMAL_ENCODING_EXT;
    remixapi_MaterialInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.pNext = &opaque;
    info.hash = nextHash++;
    info.albedoTexture = albedo.c_str();
    info.normalTexture = normal.empty() ? nullptr : normal.c_str();
    info.emissiveTexture = mask.empty() ? nullptr : mask.c_str();
    info.filterMode = 1;
    info.wrapModeU = info.wrapModeV = 1;
    if (shading) {
      const auto &s = *shading;
      if (s.mode != Shading::Lit) {
        opaque.pNext = &self;
        emission.suppressLightGeneration = s.mode == Shading::Unlit && !s.luminous;
        self.maskScale = s.maskScale;
        self.mask = s.mode == Shading::SelfIllumBaseAlpha ? REMIXAPI_SELF_ILLUMINATION_MASK_ALBEDO_ALPHA_EXT
                    : s.mode == Shading::SelfIllumMaskAlpha
                        ? REMIXAPI_SELF_ILLUMINATION_MASK_EMISSIVE_ALPHA_EXT
                        : REMIXAPI_SELF_ILLUMINATION_MASK_NONE_EXT;
        info.emissiveIntensity = 1;
        info.emissiveColorConstant = {s.tint[0], s.tint[1], s.tint[2]};
      }
      if (s.pbr.active) {
        remap.pNext = opaque.pNext;
        pbr.pNext = &remap;
        opaque.pNext = &pbr;
        opaque.roughnessConstant = s.pbr.roughness;
        opaque.metallicConstant = s.pbr.metallic;
        opaque.roughnessTexture = maps[0].empty() ? nullptr : maps[0].c_str();
        opaque.metallicTexture = maps[1].empty() ? nullptr : maps[1].c_str();
        remixapi_MaterialPbrChannelEXT *channels[] = {&remap.roughness, &remap.metallic};
        for (int i = 0; i < 2; ++i) {
          const auto &m = s.pbr.maps[i];
          *channels[i] = {m.channel, m.invert, m.curve, m.exponent, m.bias, m.scale};
        }
        if (!normal.empty()) {
          encoding.encoding = s.pbr.ssbump ? 4 : 2;
          encoding.pNext = opaque.pNext;
          opaque.pNext = &encoding;
        }
      }
    }
    if (!shading || !shading->pbr.active) {
      pbr.enabled = 0;
      pbr.pNext = opaque.pNext;
      opaque.pNext = &pbr;
    }
    remixapi_MaterialHandle handle = nullptr;
    check(api.CreateMaterial(&info, &handle), "CreateMaterial");
    materials.emplace(key.str(), handle);
    return {handle, {}};
  }

private:
  struct Texture {
    remixapi_TextureHandle handle = nullptr;
    std::wstring path;
  };
  remixapi_Interface &api;
  bool reportHashes;
  uint64_t nextHash = 0x7265706c00000001ull;
  std::map<std::string, Texture> textures;
  std::map<std::string, remixapi_MaterialHandle> materials;
  std::set<std::string> failures;
  void fail(const std::string &name, const std::string &reason) {
    if (failures.insert(name).second)
      std::fprintf(stderr, "Incomplete asset: %s (%s)\n", name.c_str(), reason.c_str());
  }
  std::wstring image(const std::string &name) {
    if (name.empty())
      return {};
    if (auto found = textures.find(name); found != textures.end())
      return found->second.path;
    auto &texture = textures[name];
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;
    std::string error;
    if (!proxy::bsp::textureImage(name, width, height, rgba, error)) {
      fail(name, error);
      return {};
    }
    remixapi_TextureInfoGenerateMipsEXT mips = {};
    mips.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_GENERATE_MIPS_EXT;
    mips.generate = 1;
    remixapi_TextureInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
    info.pNext = &mips;
    info.hash = nextHash++;
    info.width = width;
    info.height = height;
    info.depth = info.mipLevels = 1;
    info.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
    info.data = rgba.data();
    info.dataSize = rgba.size();
    check(api.CreateTexture(&info, &texture.handle), "CreateTexture");
    wchar_t path[32];
    swprintf(path, 32, L"0x%llX", static_cast<unsigned long long>(info.hash));
    texture.path = path;
    return texture.path;
  }
};
Materials *currentMaterials = nullptr;
proxy::bsp::MaterialRef lookup(uint64_t hash, const Shading *shading, const std::vector<uint64_t> *) {
  return currentMaterials->lookup(hash, shading);
}

remixapi_LightHandle light(remixapi_Interface &api, const proxy::bsp::Light &source) {
  constexpr float pi = 3.14159265f, radius = 2, reference = 100;
  remixapi_LightInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  info.hash = source.hash;
  remixapi_LightInfoSphereEXT sphere = {};
  sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
  sphere.position = {source.position[0], source.position[1], source.position[2]};
  sphere.radius = radius;
  remixapi_LightInfoDistantEXT distant = {};
  distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
  distant.direction = {source.direction[0], source.direction[1], source.direction[2]};
  distant.angularDiameterDegrees = .5f;
  float scale = pi;
  if (source.type != 2) {
    const auto &a = source.attenuation;
    const float denominator = a[0] + a[1] * reference + a[2] * reference * reference;
    if (!(denominator > 0))
      return nullptr;
    scale = reference * reference / denominator / (pi * radius * radius);
    if (source.type == 3 && source.phiDot > -1 && source.phiDot < 1) {
      const float outer = std::acos(source.phiDot);
      const float inner = std::acos(std::clamp(source.thetaDot, -1.f, 1.f));
      sphere.shaping_hasvalue = 1;
      sphere.shaping_value.direction = distant.direction;
      sphere.shaping_value.coneAngleDegrees = outer * 180 / pi;
      sphere.shaping_value.coneSoftness = outer > 0 ? std::clamp((outer - inner) / outer, 0.f, 1.f) : 0;
    }
  }
  info.radiance = {source.color[0] * scale, source.color[1] * scale, source.color[2] * scale};
  info.pNext = source.type == 2 ? static_cast<void *>(&distant) : &sphere;
  remixapi_LightHandle handle = nullptr;
  check(api.CreateLight(&info, &handle), "CreateLight");
  return handle;
}

void replay(remixapi_Interface &api, HMODULE library, HWND window, const char *map, const char *output,
            unsigned frames, const proxy::replay::Settings &settings) {
  Materials materials(api, !settings.mods.empty());
  currentMaterials = &materials;
  struct SceneLifetime {
    remixapi_Interface &api;
    std::vector<remixapi_LightHandle> lights;
    ~SceneLifetime() {
      proxy::bsp::load(api, "");
      for (auto handle : lights)
        api.DestroyLight(handle);
      currentMaterials = nullptr;
    }
  } scene{api, {}};
  if (!proxy::bsp::load(api, map))
    throw std::runtime_error("BSP load failed");
  std::array<float, 3> origin{}, angles{};
  if (!proxy::bsp::playerStart(origin.data(), angles.data()) && !settings.position)
    throw std::runtime_error("map has no player spawn and no replay.position override");
  origin[2] += 64;
  auto params = proxy::replay::camera(settings, origin, angles);
  std::printf("Camera %.3f %.3f %.3f; FOV %.3f\n", params.position.x, params.position.y, params.position.z,
              params.fovYInDegrees);
  remixapi_CameraInfo camera = {};
  camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
  camera.pNext = &params;
  auto &lights = scene.lights;
  for (const auto &source : proxy::bsp::lights())
    if (auto handle = light(api, source))
      lights.push_back(handle);
  const auto statistics = reinterpret_cast<PFN_remixapi_GetBakeStatisticsEXT>(
      GetProcAddress(library, "remixapi_GetBakeStatisticsEXT"));
  const auto backendStatistics = reinterpret_cast<PFN_remixapi_GetBackendCapabilitiesEXT>(
      GetProcAddress(library, "remixapi_GetBackendCapabilitiesEXT"));
  const auto gpuStatistics = reinterpret_cast<PFN_remixapi_GetGpuPassTimingsEXT>(
      GetProcAddress(library, "remixapi_GetGpuPassTimingsEXT"));
  const auto denoisingStatistics = reinterpret_cast<PFN_remixapi_GetDenoisingStatisticsEXT>(
      GetProcAddress(library, "remixapi_GetDenoisingStatisticsEXT"));
  proxy::replay::TimingWindow measurement(settings.measureFrames, settings.warmupFrames,
                                          settings.warmupSeconds);
  if (measurement.enabled()) {
    if (!backendStatistics || !gpuStatistics || (settings.waitForBake && !statistics))
      throw std::runtime_error("Runtime lacks GPU measurement statistics");
    check(api.SetConfigVariable("rtx.native.gpuPassTimers", "1"), "Enable GPU timestamps");
  }
  const char *profile = std::getenv("REMIX_CONFIG_DIR");
  const std::filesystem::path diagnostics =
      profile ? std::filesystem::path(profile) : std::filesystem::path();
  const auto previousProbeReport = lastWrite(diagnostics / "probes.json");
  bool requestedProbes = false;
  unsigned measurementEnd = ~0u;
  uint32_t lastState = ~0u;
  bool reportedPendingInputs = false;
  const auto start = Clock::now();
  for (unsigned frame = 0; frame < frames; ++frame) {
    MSG message;
    while (PeekMessageW(&message, window, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    check(api.SetupCamera(&camera), "SetupCamera");
    for (auto handle : lights)
      check(api.DrawLightInstance(handle), "DrawLightInstance");
    proxy::bsp::submit(lookup);
    if (materials.complete())
      proxy::bsp::publishBakeScene(lights);
    if (frame == frames - 3 || (measurementEnd != ~0u && frame == measurementEnd + 1))
      check(api.RequestPresentedScreenshot(output), "RequestPresentedScreenshot");
    check(api.Present(nullptr), "Present");
    remixapi_BakeStatisticsEXT stats = {};
    stats.structSize = sizeof(stats);
    if (statistics) {
      check(statistics(&stats), "GetBakeStatistics");
      if (measurement.enabled() && settings.waitForBake && stats.state == REMIXAPI_BAKE_FAILED_EXT)
        throw std::runtime_error(std::string("Map bake failed: ") + stats.status);
      if (stats.active && profile && !requestedProbes) {
        std::ofstream(diagnostics / "probes.request");
        requestedProbes = true;
      }
      if (stats.state != lastState || frame % 120 == 0) {
        std::printf("Frame %u, %.2fs: bake %u, active %u, progress %u/%u; %s\n", frame,
                    std::chrono::duration<double>(Clock::now() - start).count(), stats.state, stats.active,
                    stats.completed, stats.probes, stats.status);
        std::fflush(stdout);
        lastState = stats.state;
      }
    }
    if (measurement.enabled() && !measurement.complete()) {
      remixapi_BackendCapabilitiesEXT backend{};
      backend.structSize = sizeof(backend);
      check(backendStatistics(&backend), "GetBackendCapabilities");
      remixapi_GpuPassTimingsEXT timing{};
      timing.structSize = sizeof(timing);
      check(gpuStatistics(&timing), "GetGpuPassTimings");
      bool ready = materials.complete() && backend.activeBackend == backend.requestedBackend &&
                   (backend.activeBackend != 2 || !settings.waitForBake || stats.active);
      if (backend.requestedBackend == 1) {
        if (!denoisingStatistics)
          throw std::runtime_error("Runtime lacks denoising readiness statistics");
        remixapi_DenoisingStatisticsEXT denoising{};
        denoising.structSize = sizeof(denoising);
        check(denoisingStatistics(&denoising), "GetDenoisingStatistics");
        ready &= denoising.activeDenoiser == denoising.requestedDenoiser &&
                 denoising.activeTaa == denoising.requestedTaa;
      }
      if (!ready && frame % 120 == 0)
        std::printf("Measurement waiting: backend %u requested %u; %s\n", backend.activeBackend,
                    backend.requestedBackend, backend.unavailableReason);
      if (!ready && !reportedPendingInputs &&
          std::chrono::duration<double>(Clock::now() - start).count() > 5) {
        proxy::bsp::dumpStatistics(stdout);
        std::fflush(stdout);
        reportedPendingInputs = true;
      }
      measurement.observe(ready, backend.activeBackend, backend.activeBackend == 2 ? stats.activeHash : 0,
                          timing);
      if (measurement.complete()) {
        measurementEnd = frame;
        if (const char *prefix = std::getenv("REMIX_DEBUG_BAKE_CAPTURE_PREFIX")) {
          std::ofstream request(std::string(prefix) + ".frame.request");
          if (!request)
            std::fprintf(stderr, "Cannot request the optional frame input dump\n");
        }
        std::printf("GPU measurement complete: %zu distinct frames after %u warmup frames\n",
                    measurement.samples(), measurement.warmedFrames());
      }
    }
    const unsigned drain = std::getenv("REMIX_DEBUG_BAKE_CAPTURE_PREFIX") ? 8 : 3;
    if (measurementEnd != ~0u && frame >= measurementEnd + drain) {
      // Runtime file requests are serviced every 30 presents. A short timing
      // window can finish before that poll; drain the requested probe report
      // as well as the screenshot, without including either in measurements.
      const bool pendingProbes = profile && requestedProbes &&
                                 lastWrite(diagnostics / "probes.json") == previousProbeReport;
      if (!pendingProbes)
        break;
      if (frame - measurementEnd >= 60)
        throw std::runtime_error("Published reflection probe report did not arrive within 60 presents");
    }
    if (!measurement.enabled())
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (measurement.enabled())
    measurement.write(output, map);
  if (profile && requestedProbes && lastWrite(diagnostics / "probes.json") != previousProbeReport) {
    auto report = std::filesystem::path(output);
    report.replace_extension(".probes.json");
    std::filesystem::copy_file(diagnostics / "probes.json", report,
                               std::filesystem::copy_options::overwrite_existing);
    std::printf("Published reflection probe report: %s\n", report.string().c_str());
  }
  proxy::bsp::dumpStatistics(stdout);
  if (!settings.mods.empty()) {
    const auto diagnostic = reinterpret_cast<decltype(&remixapi_GetUSDModsDiagnosticEXT)>(
        GetProcAddress(library, "remixapi_GetUSDModsDiagnosticEXT"));
    char message[2048]{};
    if (!diagnostic) throw std::runtime_error("Runtime lacks USD mod diagnostics");
    check(diagnostic(message, sizeof(message)), "USD mod diagnostics");
    if (message[0]) throw std::runtime_error(std::string("USD mod load: ") + message);
    std::printf("USD mod directory: %s (no loader diagnostic)\n", settings.mods.c_str());
  }
  std::printf("Static VMT replay only: engine material proxies, entities, particles and UI are absent.\n");
}
} // namespace

namespace {
LRESULT CALLBACK replayWindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_GETMINMAXINFO) {
    // Diagnostic captures may exceed the desktop size. Settings bound each
    // client dimension to 4096; leave space here for the non-client frame.
    auto &limits = *reinterpret_cast<MINMAXINFO *>(lparam);
    limits.ptMaxTrackSize = {8192, 8192};
    return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace

int main(int argc, char **argv) {
  if (argc < 5) {
    std::fprintf(stderr,
                 "Usage: bsp-replay runtime.dll content-root map output.png [frames=600] [settings.cfg]\n");
    return 2;
  }
  SetEnvironmentVariableA("OPENREMIX_PROXY_CONTENT_ROOT", argv[2]);
  const auto path = std::filesystem::absolute(argv[1]).wstring();
  SetDllDirectoryW(std::filesystem::path(path).parent_path().c_str());
  WNDCLASSW windowClass{};
  windowClass.lpfnWndProc = replayWindowProcedure;
  windowClass.hInstance = GetModuleHandleW(nullptr);
  windowClass.lpszClassName = L"OpenRemixBspReplay";
  if (!RegisterClassW(&windowClass))
    return 1;
  HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"BSP replay", WS_OVERLAPPEDWINDOW, 0, 0, 640,
                                360, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!window)
    return 1;
  remixapi_Interface api = {};
  remixapi_HMODULE library = nullptr;
  int result = 0;
  const auto previousScreenshot = lastWrite(argv[4]);
  try {
    check(remixapi_lib_loadRemixDllAndInitialize(path.c_str(), &api, &library), "Load runtime");
    remixapi_StartupInfo startup = {};
    startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
    startup.hwnd = window;
    check(api.Startup(&startup), "Startup");
    check(api.SetConfigVariable("rtx.native.backend", "forwardplus"), "Select Forward+");
    check(api.SetConfigVariable("rtx.native.pathtrace.overrideLight", "0"), "Disable profile sun");
    check(api.SetConfigVariable("rtx.native.forward.sceneScale", "0.0254"), "Set Source scale");
    const auto settings = argc > 6 ? proxy::replay::configure(api, argv[6]) : proxy::replay::Settings{};
    if (!settings.mods.empty()) {
      const auto configure = reinterpret_cast<decltype(&remixapi_ConfigureUSDModsEXT)>(
          GetProcAddress(library, "remixapi_ConfigureUSDModsEXT"));
      if (!configure) throw std::runtime_error("Runtime lacks USD mod loading");
      remixapi_USDModsOptionsEXT mods{};
      mods.structSize = sizeof(mods); mods.version = 1; mods.directory = settings.mods.c_str();
      mods.hostMetersPerUnit = .01; mods.hostUpAxis = 2; mods.watch = 0;
      check(configure(&mods), "Configure USD mods");
    }
    RECT bounds{0, 0, LONG(settings.width), LONG(settings.height)};
    if (!AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW, FALSE) ||
        !SetWindowPos(window, nullptr, 0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top,
                      SWP_NOMOVE | SWP_NOZORDER))
      throw std::runtime_error("Cannot size replay window");
    RECT client{};
    if (!GetClientRect(window, &client) || client.right != LONG(settings.width) ||
        client.bottom != LONG(settings.height))
      throw std::runtime_error("Replay client size differs from requested resolution: " +
                               std::to_string(client.right) + "x" + std::to_string(client.bottom));
    ShowWindow(window, SW_SHOWNOACTIVATE);
    std::printf("Replay target %u x %u\n", settings.width, settings.height);
    replay(api, library, window, argv[3], argv[4], argc > 5 ? std::max(4, std::atoi(argv[5])) : 600,
           settings);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    result = 1;
  }
  if (library)
    remixapi_lib_shutdownAndUnloadRemixDll(&api, library);
  if (!result && lastWrite(argv[4]) == previousScreenshot) {
    std::fprintf(stderr, "Replay produced no screenshot\n");
    result = 1;
  }
  DestroyWindow(window);
  return result;
}
