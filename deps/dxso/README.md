# dxso, standalone

DXVK 2.6.2's D3D9 shader compiler (`src/dxso`), SPIR-V builder (`src/spirv`) and the parts of `src/util` they use,
vendored so the proxy can turn the game's own vertex and pixel shaders (screen-space effects) into SPIR-V that
OpenRemix replays. Licence: zlib (LICENSE); SPIR-V headers under include/spirv (KhronosGroup/SPIRV-Headers,
include/spirv/LICENSE); DXVK's native Windows/D3D9 headers under include/native (for a Linux build of the tool).

Shims replace the DXVK internals the compiler was written against, keeping the names it includes:
`src/d3d9/d3d9_include.h`, `d3d9_constant_set.h`, `d3d9_state.h` (the render-state push block and shared
pixel stage enum), `d3d9_fixed_function.{h,cpp}` (the fog/alpha-test/point-size/spec-UBO helpers only),
`d3d9_options.h`, `d3d9_device.h`, `d3d9_util.h`, `src/dxvk/dxvk_shader.h` (the compiled shader: SPIR-V and its
bindings), `dxvk_device.h`, `src/dxbc/dxbc_include.h`. `dxso_options.cpp` keeps only the default constructor.
Verbatim: `d3d9_caps.h`, `d3d9_constant_layout.h`, `d3d9_spec_constants.h`, `dxbc_tag.h`, `dxvk_hash.h`.

`tools/dxso_tool.cpp` compiles one shader file to SPIR-V and prints its interface (Linux build:
`g++ -std=c++17 -I src -I include -I include/native/windows -I include/native/directx -I <vulkan headers> ...`).
