// Smoke test: drives the native sample scene plus payloads that exercise every
// transport path (inline, DATA record, blob, Indirect record) into a window
// owned by this process. Built as x86 it goes through remix_bridge.dll; built
// as x64 it calls remix.dll directly, producing a reference image.
//
// usage: bridge_smoke [frames] [fixed-time-seconds] [screenshot.png]
#define REMIX_ALLOW_X86
#include <remix/remix_c.h>
#include <remix/remix_native_ext.h>

#include "scene.h"
#include "smoke_shaders.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

int fail(const char *what, remixapi_ErrorCode code) {
  std::printf("FAIL %s: %d\n", what, int(code));
  return 1;
}

// A mesh with more surfaces than the runtime's former 4096 cap: its record
// exceeds a quarter of an 8 MiB ring and must travel as a blob plus an
// Indirect record.
remixapi_ErrorCode createManySurfaceMesh(remixapi_Interface &api, remixapi_MaterialHandle material,
                                         remixapi_MeshHandle &out, uint32_t surfaces) {
  std::vector<remixapi_HardcodedVertex> vertices(12);
  vertices[0].position[0] = -0.2f;
  vertices[1].position[0] = 0.2f;
  vertices[2].position[1] = 0.3f;
  for (auto &v : vertices)
    v.color = 0xff40c0ff;
  const uint32_t indices[3] = {0, 1, 2};
  std::vector<remixapi_MeshInfoSurfaceTriangles> list(surfaces);
  for (auto &s : list) {
    s.vertices_values = vertices.data();
    s.vertices_count = vertices.size();
    s.indices_values = indices;
    s.indices_count = 3;
    s.material = material;
  }
  remixapi_MeshInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
  info.hash = 0x300;
  info.surfaces_values = list.data();
  info.surfaces_count = surfaces;
  return api.CreateMesh(&info, &out);
}
} // namespace

int main(int argc, char **argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  const unsigned frames = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 600;
  const float fixedTime = argc > 2 ? static_cast<float>(std::atof(argv[2])) : -1.0f;
  const char *screenshot = argc > 3 ? argv[3] : "bridge_smoke.png";
  SetEnvironmentVariableA("OPENREMIX_BRIDGE_RING_MB", "8");

  WNDCLASSW wc = {};
  wc.lpfnWndProc = windowProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"bridge_smoke";
  RegisterClassW(&wc);
  HWND hwnd = CreateWindowW(L"bridge_smoke", L"OpenRemix bridge smoke test (x86)", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                            100, 100, 1280, 720, nullptr, nullptr, wc.hInstance, nullptr);

  remixapi_Interface api = {};
  remixapi_HMODULE lib = nullptr;
#if defined(_WIN64)
  auto r = remixapi_lib_loadRemixDllAndInitialize(L"remix.dll", &api, &lib);
#else
  auto r = remixapi_lib_loadRemixDllAndInitialize(L"remix_bridge.dll", &api, &lib);
#endif
  if (r)
    return fail("InitializeLibrary", r);

  remixapi_StartupInfo startup = {};
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
  startup.hwnd = hwnd;
  if ((r = api.Startup(&startup)))
    return fail("Startup", r);
  if ((r = api.SetConfigVariable("rtx.native.backend", "raster")))
    return fail("SetConfigVariable", r);

  SampleScene scene;
  if ((r = scene.create(api)))
    return fail("scene.create", r);

  // DATA record path (exactly ring/8) and blob path (> ring/8).
  std::vector<uint32_t> medium(512 * 512, 0xff8080ff), large(2048 * 2048, 0xff80ff80);
  remixapi_TextureInfo tex = {};
  tex.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
  tex.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
  tex.depth = tex.mipLevels = 1;
  remixapi_TextureHandle mediumTex = nullptr, largeTex = nullptr;
  tex.hash = 0x201;
  tex.width = tex.height = 512;
  tex.data = medium.data();
  tex.dataSize = medium.size() * 4;
  if ((r = api.CreateTexture(&tex, &mediumTex)))
    return fail("CreateTexture(medium)", r);
  tex.hash = 0x202;
  tex.width = tex.height = 2048;
  tex.data = large.data();
  tex.dataSize = large.size() * 4;
  if ((r = api.CreateTexture(&tex, &largeTex)))
    return fail("CreateTexture(large)", r);

  remixapi_MeshHandle many = nullptr;
  if ((r = createManySurfaceMesh(api, scene.material, many, 5000)))
    return fail("CreateMesh(many)", r);

  // remix_native_ext.h named exports: scene instances, per-instance
  // materials (a handle inside a pNext chain), dynamic texture and UI region
  // updates (strided sub-rectangles), host-pass getters.
  auto sym = [&](const char *name) { return reinterpret_cast<void *>(GetProcAddress(lib, name)); };
  auto addScene = reinterpret_cast<PFN_remixapi_AddSceneInstancesEXT>(sym("remixapi_AddSceneInstancesEXT"));
  auto changeScene = reinterpret_cast<PFN_remixapi_ChangeSceneInstancesEXT>(sym("remixapi_ChangeSceneInstancesEXT"));
  auto removeScene = reinterpret_cast<PFN_remixapi_RemoveSceneInstancesEXT>(sym("remixapi_RemoveSceneInstancesEXT"));
  auto drawScene = reinterpret_cast<PFN_remixapi_DrawSceneInstancesEXT>(sym("remixapi_DrawSceneInstancesEXT"));
  auto updateTexture = reinterpret_cast<PFN_remixapi_UpdateTextureEXT>(sym("remixapi_UpdateTextureEXT"));
  auto updateUIRegion =
      reinterpret_cast<PFN_remixapi_UpdateUITextureRegionEXT>(sym("remixapi_UpdateUITextureRegionEXT"));
  auto passesEnabled = reinterpret_cast<PFN_remixapi_GetHostPassesEnabledEXT>(sym("remixapi_GetHostPassesEnabledEXT"));
  auto passStats = reinterpret_cast<PFN_remixapi_GetHostPassStatisticsEXT>(sym("remixapi_GetHostPassStatisticsEXT"));
  auto textureStats = reinterpret_cast<PFN_remixapi_GetTextureStatisticsEXT>(sym("remixapi_GetTextureStatisticsEXT"));
  auto configDefault = reinterpret_cast<PFN_remixapi_SetConfigDefaultEXT>(sym("remixapi_SetConfigDefaultEXT"));
  if (!addScene || !changeScene || !removeScene || !drawScene || !updateTexture || !updateUIRegion || !passesEnabled ||
      !passStats || !textureStats || !configDefault)
    return fail("native ext exports", REMIXAPI_ERROR_CODE_GET_PROC_ADDRESS_FAILURE);
  std::printf("SetConfigDefaultEXT %d\n", int(configDefault("rtx.native.meshes.refreshInPlace", "1")));

  // Host passes: a solid quad drawn by registered shaders (push constants, a
  // uniform buffer, vertices, indices) and a host target copied into the frame.
  auto registerShader = reinterpret_cast<PFN_remixapi_RegisterHostShaderEXT>(sym("remixapi_RegisterHostShaderEXT"));
  auto destroyShader = reinterpret_cast<PFN_remixapi_DestroyHostShaderEXT>(sym("remixapi_DestroyHostShaderEXT"));
  auto submitPasses = reinterpret_cast<PFN_remixapi_SubmitHostPassesEXT>(sym("remixapi_SubmitHostPassesEXT"));
  auto updateTarget = reinterpret_cast<PFN_remixapi_UpdateHostTargetEXT>(sym("remixapi_UpdateHostTargetEXT"));
  if (!registerShader || !destroyShader || !submitPasses || !updateTarget)
    return fail("host pass exports", REMIXAPI_ERROR_CODE_GET_PROC_ADDRESS_FAILURE);
  if ((r = api.SetConfigVariable("rtx.native.hostPasses", "1")))
    return fail("SetConfigVariable(hostPasses)", r);
  remixapi_HostShaderInfoEXT shaderInfo = {};
  shaderInfo.sType = REMIXAPI_STRUCT_TYPE_HOST_SHADER_INFO_EXT;
  shaderInfo.stage = REMIXAPI_HOST_SHADER_VERTEX_EXT;
  shaderInfo.code = kQuadVert;
  shaderInfo.codeBytes = sizeof(kQuadVert);
  shaderInfo.pushConstantBytes = 16;
  remixapi_HostShaderHandleEXT quadVert = 0, quadFrag = 0;
  if ((r = registerShader(&shaderInfo, &quadVert)))
    return fail("RegisterHostShaderEXT(vertex)", r);
  const remixapi_HostShaderBindingEXT ubo = {0, REMIXAPI_HOST_BINDING_UNIFORM_BUFFER_EXT};
  shaderInfo.stage = REMIXAPI_HOST_SHADER_FRAGMENT_EXT;
  shaderInfo.code = kQuadFrag;
  shaderInfo.codeBytes = sizeof(kQuadFrag);
  shaderInfo.bindings = &ubo;
  shaderInfo.bindingCount = 1;
  if ((r = registerShader(&shaderInfo, &quadFrag)))
    return fail("RegisterHostShaderEXT(fragment)", r);
  const remixapi_HostTargetHandleEXT magentaTarget = 0x1234;
  const uint32_t magenta[16] = {0xffff00ff, 0xffff00ff, 0xffff00ff, 0xffff00ff, 0xffff00ff, 0xffff00ff,
                                0xffff00ff, 0xffff00ff, 0xffff00ff, 0xffff00ff, 0xffff00ff, 0xffff00ff,
                                0xffff00ff, 0xffff00ff, 0xffff00ff, 0xffff00ff};
  if ((r = updateTarget(magentaTarget, 4, 4, 37 /* VK_FORMAT_R8G8B8A8_UNORM */, magenta, sizeof(magenta))))
    return fail("UpdateHostTargetEXT", r);

  remixapi_MaterialInfoOpaqueEXT redOpaque = {};
  redOpaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
  redOpaque.albedoConstant = {1, 0.1f, 0.1f};
  redOpaque.opacityConstant = 1;
  redOpaque.alphaTestType = 7; // always; 0 would discard every pixel
  remixapi_MaterialInfo redInfo = {};
  redInfo.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
  redInfo.pNext = &redOpaque;
  redInfo.hash = 0x400;
  remixapi_MaterialHandle red = nullptr;
  if ((r = api.CreateMaterial(&redInfo, &red)))
    return fail("CreateMaterial(red)", r);

  // A dynamic texture (grey) on a material; at frame 5 its middle becomes a
  // blue/yellow pattern from a sub-rectangle of a wider buffer.
  std::vector<uint32_t> grey(64 * 64, 0xff808080);
  remixapi_TextureInfoDynamicEXT dynamicExt = {};
  dynamicExt.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_DYNAMIC_EXT;
  dynamicExt.dynamic = 1;
  tex.pNext = &dynamicExt;
  tex.hash = 0x500;
  tex.width = tex.height = 64;
  tex.data = grey.data();
  tex.dataSize = grey.size() * 4;
  remixapi_TextureHandle dynamicTex = nullptr;
  if ((r = api.CreateTexture(&tex, &dynamicTex)))
    return fail("CreateTexture(dynamic)", r);
  tex.pNext = nullptr;
  std::vector<uint32_t> pattern(128 * 64);
  for (uint32_t y = 0; y < 64; ++y)
    for (uint32_t x = 0; x < 128; ++x)
      pattern[y * 128 + x] = ((x / 4 + y / 4) % 2) ? 0xffff4020 : 0xff20e0ff;
  remixapi_MaterialInfoOpaqueEXT whiteOpaque = redOpaque;
  whiteOpaque.albedoConstant = {1, 1, 1};
  remixapi_MaterialInfo dynamicInfo = redInfo;
  dynamicInfo.pNext = &whiteOpaque;
  dynamicInfo.hash = 0x501;
  dynamicInfo.albedoTexture = L"0x500";
  remixapi_MaterialHandle dynamicMat = nullptr;
  if ((r = api.CreateMaterial(&dynamicInfo, &dynamicMat)))
    return fail("CreateMaterial(dynamic)", r);

  // A scene instance: a small cube the runtime keeps, moved every frame.
  remixapi_InstanceInfo sceneInfo = {};
  sceneInfo.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
  sceneInfo.mesh = scene.mesh;
  sceneInfo.transform.matrix[0][0] = sceneInfo.transform.matrix[1][1] = sceneInfo.transform.matrix[2][2] = 0.5f;
  uint64_t sceneInstance = 0;
  if ((r = addScene(&sceneInfo, 1, &sceneInstance)) || !sceneInstance)
    return fail("AddSceneInstancesEXT", r);

  remixapi_WindowState ws = {};
  if ((r = api.GetWindowState(&ws)))
    return fail("GetWindowState", r);
  std::printf("drawable %ux%u focused %u\n", ws.drawableWidth, ws.drawableHeight, ws.focused);
  std::printf("ui state %d\n", int(api.GetUIState()));

  const auto start = std::chrono::steady_clock::now();
  unsigned frame = 0;
  MSG msg;
  bool quit = false;
  for (; frame < frames && !quit; ++frame) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT)
        quit = true;
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    RECT rc;
    GetClientRect(hwnd, &rc);
    const float t = fixedTime >= 0 ? fixedTime
                                   : std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
    if ((r = scene.draw(api, t, rc.right - rc.left, rc.bottom - rc.top)))
      return fail("scene.draw", r);
    remixapi_InstanceInfo inst = {};
    inst.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    inst.mesh = many;
    inst.doubleSided = 1;
    inst.transform.matrix[0][0] = inst.transform.matrix[1][1] = inst.transform.matrix[2][2] = 1;
    inst.transform.matrix[0][3] = -2.5f;
    inst.transform.matrix[2][3] = 7;
    if ((r = api.DrawInstance(&inst)))
      return fail("DrawInstance(many)", r);

    remixapi_InstanceInfoMaterialEXT override = {};
    override.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_MATERIAL_EXT;
    override.material = red;
    remixapi_InstanceInfo redCube = {};
    redCube.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    redCube.pNext = &override;
    redCube.mesh = scene.mesh;
    redCube.transform.matrix[0][0] = redCube.transform.matrix[1][1] = redCube.transform.matrix[2][2] = 0.6f;
    redCube.transform.matrix[0][3] = 2.2f;
    redCube.transform.matrix[1][3] = -1.6f;
    redCube.transform.matrix[2][3] = 6;
    if ((r = api.DrawInstance(&redCube)))
      return fail("DrawInstance(override)", r);
    override.material = dynamicMat;
    redCube.transform.matrix[0][3] = -2.2f;
    if ((r = api.DrawInstance(&redCube)))
      return fail("DrawInstance(dynamic)", r);

    if (frame == 5) {
      // 32x32 at (16, 16), from column 8 of a 128-texel-wide buffer.
      if ((r = updateTexture(dynamicTex, 16, 16, 32, 32, REMIXAPI_FORMAT_R8G8B8A8_SRGB, pattern.data() + 8,
                             128 * 4)))
        return fail("UpdateTextureEXT", r);
      if ((r = updateUIRegion(1, 2, 2, 4, 4, REMIXAPI_FORMAT_R8G8B8A8_SRGB, pattern.data() + 8, 128 * 4)))
        return fail("UpdateUITextureRegionEXT", r);
    }

    remixapi_SceneInstanceChangeEXT move = {};
    move.instance = sceneInstance;
    move.fields = REMIXAPI_SCENE_INSTANCE_TRANSFORM_EXT;
    move.transform = sceneInfo.transform;
    move.transform.matrix[0][3] = -2.0f + 0.5f * std::sin(t);
    move.transform.matrix[1][3] = 1.6f;
    move.transform.matrix[2][3] = 6;
    if ((r = changeScene(&move, 1)))
      return fail("ChangeSceneInstancesEXT", r);
    if ((r = drawScene(nullptr)))
      return fail("DrawSceneInstancesEXT", r);

    const float quad[8] = {0.55f, -0.95f, 0.95f, -0.95f, 0.95f, -0.55f, 0.55f, -0.55f};
    const uint32_t quadIndices[6] = {0, 1, 2, 0, 2, 3};
    const float color[4] = {0, 1, 1, 1}, tint[4] = {1, 0.5f, 1, 1};
    remixapi_HostPassBufferEXT buffer = {0, tint, sizeof(tint)};
    remixapi_HostPassVertexBindingEXT binding = {quad, sizeof(quad), 8};
    remixapi_HostPassAttributeEXT attribute = {0, 0, 103 /* VK_FORMAT_R32G32_SFLOAT */, 0};
    remixapi_HostPassEXT passes[2] = {};
    passes[0].kind = REMIXAPI_HOST_PASS_DRAW_EXT;
    passes[0].vertexShader = quadVert;
    passes[0].fragmentShader = quadFrag;
    passes[0].buffers = &buffer;
    passes[0].bufferCount = 1;
    passes[0].pushConstants = color;
    passes[0].pushConstantBytes = sizeof(color);
    passes[0].vertexBindings = &binding;
    passes[0].vertexBindingCount = 1;
    passes[0].attributes = &attribute;
    passes[0].attributeCount = 1;
    passes[0].indices = quadIndices;
    passes[0].indexCount = 6;
    passes[0].colorWriteMask = 0xf;
    passes[1].kind = REMIXAPI_HOST_PASS_COPY_EXT;
    passes[1].source = magentaTarget;
    passes[1].targetRect[0] = 40;
    passes[1].targetRect[1] = 40;
    passes[1].targetRect[2] = 100;
    passes[1].targetRect[3] = 100;
    remixapi_HostPassListEXT passList = {};
    passList.sType = REMIXAPI_STRUCT_TYPE_HOST_PASS_LIST_EXT;
    passList.passes = passes;
    passList.passCount = 2;
    if ((r = submitPasses(&passList)))
      return fail("SubmitHostPassesEXT", r);
    if (frame == frames / 2) {
      char path[MAX_PATH];
      GetFullPathNameA(screenshot, MAX_PATH, path, nullptr);
      api.RequestPresentedScreenshot(path); // the runtime requires an absolute path
    }
    if ((r = api.Present(nullptr)))
      return fail("Present", r);
  }
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

  remixapi_FramePerformanceEXT perf = {};
  perf.structSize = sizeof(perf);
  auto getPerf = reinterpret_cast<PFN_remixapi_GetFramePerformanceEXT>(
      sym("remixapi_GetFramePerformanceEXT"));
  r = getPerf ? getPerf(&perf) : REMIXAPI_ERROR_CODE_GET_PROC_ADDRESS_FAILURE;
  std::printf("frame performance: %d presentations=%llu present=%.3fms\n", int(r),
              static_cast<unsigned long long>(perf.presentations), perf.presentMilliseconds);

  remixapi_Bool hostPasses = 0;
  r = passesEnabled(&hostPasses);
  std::printf("host passes enabled: %d (%u)\n", int(r), hostPasses);
  remixapi_HostPassStatisticsEXT hp = {};
  hp.structSize = sizeof(hp);
  r = passStats(&hp);
  std::printf("host pass statistics: %d passes=%u draws=%u copies=%u skipped=%u shaders=%u targets=%u\n", int(r),
              hp.passes, hp.draws, hp.copies, hp.skipped, hp.shaders, hp.targets);
  remixapi_TextureStatisticsEXT ts = {};
  ts.structSize = sizeof(ts);
  r = textureStats(&ts);
  std::printf("texture statistics: %d updateCalls=%llu updates=%llu updateBytes=%llu\n", int(r),
              static_cast<unsigned long long>(ts.updateCalls), static_cast<unsigned long long>(ts.updates),
              static_cast<unsigned long long>(ts.updateBytes));

  auto asyncErrors = reinterpret_cast<uint32_t(REMIXAPI_CALL *)(void)>(
      sym("openremix_bridge_GetAsyncErrorCount"));
  const uint32_t errors = asyncErrors ? asyncErrors() : 0; // absent when calling remix.dll directly
  std::printf("frames=%u fps=%.1f asyncErrors=%u\n", frame, frame / seconds, errors);

  removeScene(&sceneInstance, 1);
  destroyShader(quadVert);
  destroyShader(quadFrag);
  api.DestroyMaterial(red);
  api.DestroyMaterial(dynamicMat);
  api.DestroyTexture(dynamicTex);
  api.DestroyMesh(many);
  api.DestroyTexture(mediumTex);
  api.DestroyTexture(largeTex);
  r = remixapi_lib_shutdownAndUnloadRemixDll(&api, lib);
  std::printf("shutdown %d\n", int(r));
  DestroyWindow(hwnd);
  return errors == 0 && r == REMIXAPI_ERROR_CODE_SUCCESS ? 0 : 1;
}
