project('gmodrtx', ['cpp', 'c'], version: '0.1.0',
        default_options: ['cpp_std=c++17', 'warning_level=2', 'buildtype=release', 'b_ndebug=if-release'])

# 64-bit only (Garry's Mod x86-64 branch): configure with cross/x86_64-w64-mingw32.txt.
if host_machine.cpu_family() != 'x86_64'
  error('the shaderapidx9 proxy targets Garry\'s Mod x64')
endif

remix_inc = get_option('openremix_include')
if remix_inc == ''
  error('set -Dopenremix_include=<openremix>/public/include')
endif
# SDL3 headers for the panel's input events (the runtime ships SDL3.dll).
sdl_inc = get_option('sdl_include')
if sdl_inc == ''
  sdl_inc = remix_inc / '..' / '..' / 'subprojects' / 'SDL3-3.4.14' / 'include'
endif

subdir('deps/dxso')

# The proxy's own host-pass shaders (src/shaders), SPIR-V as C arrays.
glslang = find_program('glslangValidator')
host_shaders = []
foreach shader : ['host_depth.vert', 'host_depth.frag', 'host_frame_depth.frag']
  host_shaders += custom_target(shader + '.inc',
    input: 'src/shaders' / shader, output: shader + '.inc',
    command: [glslang, '-V', '--vn', shader.replace('.', '_'), '-o', '@OUTPUT@', '@INPUT@'])
endforeach

shared_library('shaderapidx9',
  'src/main.cpp', 'src/vtable_proxy.cpp', 'src/d3d_hooks.cpp', 'src/remix_output.cpp', 'src/texture_formats.cpp',
  'src/dxvk_hash.cpp', 'src/texture_categories.cpp', 'src/source_fs.cpp', 'src/bsp_world.cpp', 'src/engine_hooks.cpp',
  'src/remix_ui.cpp', 'src/engine_patches.cpp', 'src/engine_lights.cpp', 'src/engine_shadows.cpp',
  'src/engine_materials.cpp', 'src/material_shading.cpp', 'src/engine_nocull.cpp', 'src/pbr_rules.cpp',
  'src/vertex_program.cpp', 'src/particles.cpp', 'src/diag.cpp', 'src/lzma_source.cpp', 'src/host_passes.cpp', 'src/bake_sky.cpp',
  'src/bake_sky_cache.cpp', 'src/source_lights.cpp', 'src/styled_lights.cpp', 'deps/lzma/LzmaDec.c', host_shaders,
  include_directories: include_directories(remix_inc, sdl_inc, 'deps/xxhash'),
  dependencies: [dxso_dep],
  name_prefix: '',
  vs_module_defs: 'shaderapidx9.def',
  link_args: ['-static', '-static-libgcc', '-static-libstdc++'],
)

bake_charts_test = executable('bsp-bake-charts-test', 'tests/bake_charts.cpp',
  include_directories: include_directories('src'),
  link_args: ['-static', '-static-libgcc', '-static-libstdc++'])
test('BSP surface lightmap chart mapping', bake_charts_test)

source_fs_test = executable('source-fs-test', 'tests/source_fs.cpp', 'src/source_fs.cpp',
  'src/lzma_source.cpp', 'deps/lzma/LzmaDec.c',
  include_directories: include_directories('src'),
  link_args: ['-static', '-static-libgcc', '-static-libstdc++'])
test('Source loose and VPK relative model paths', source_fs_test,
  args: [meson.current_build_dir() / 'source-fs-readback'])

# A diagnostic map host: reads installed content without injecting into the game.
executable('bsp-replay', 'tools/bsp_replay.cpp', 'tools/replay_settings.cpp', 'tools/replay_timing.cpp',
  'src/bsp_world.cpp', 'src/source_fs.cpp', 'src/lzma_source.cpp',
  'src/texture_formats.cpp', 'src/texture_categories.cpp', 'src/pbr_rules.cpp',
  'src/material_shading.cpp', 'src/source_lights.cpp', 'src/styled_lights.cpp', 'deps/lzma/LzmaDec.c',
  include_directories: include_directories('src', remix_inc, 'deps/xxhash'),
  dependencies: meson.get_compiler('cpp').find_library('user32'),
  link_args: ['-static', '-static-libgcc', '-static-libstdc++'])

replay_settings_test = executable('replay-settings-test', 'tests/replay_settings.cpp', 'tools/replay_settings.cpp',
  include_directories: include_directories('tools', remix_inc),
  link_args: ['-static', '-static-libgcc', '-static-libstdc++'])
test('Replay camera settings and Source orientation', replay_settings_test)
replay_timing_test = executable('replay-timing-test', 'tests/replay_timing.cpp', 'tools/replay_timing.cpp',
  include_directories: include_directories('tools', remix_inc),
  link_args: ['-static', '-static-libgcc', '-static-libstdc++'])
test('Replay steady-state GPU timing window', replay_timing_test)

shader_bytecode_test = executable('shader-bytecode-test', 'tests/shader_bytecode.cpp',
  include_directories: include_directories('src'),
  link_args: ['-static', '-static-libgcc', '-static-libstdc++'])
test('Bounded D3D9 shader bytecode capture', shader_bytecode_test)

d3d_method_test = executable('d3d-method-test', 'tests/d3d_method.cpp',
  include_directories: include_directories('src'),
  link_args: ['-static', '-static-libgcc', '-static-libstdc++'])
test('D3D9 resource methods across distinct vtables', d3d_method_test)

bake_sky_test = executable('bake-sky-test', 'tests/bake_sky.cpp', 'src/bake_sky.cpp', 'src/bake_sky_cache.cpp',
  include_directories: include_directories('src', remix_inc, 'deps/xxhash'),
  link_args: ['-static', '-static-libgcc', '-static-libstdc++'])
test('Map-load bake sky ownership and animation isolation', bake_sky_test)
bake_sky_cache_test = executable('bake-sky-cache-test', 'tests/bake_sky_cache.cpp', 'src/bake_sky_cache.cpp',
  include_directories: include_directories('src', remix_inc, 'deps/xxhash'),
  link_args: ['-static', '-static-libgcc', '-static-libstdc++'])
test('Bake sky capture-noise reuse and cache invalidation', bake_sky_cache_test,
  args: [meson.current_build_dir() / 'bake-sky-cache-fixtures'])
device_window_test = executable('device-window-test', 'tests/device_window.cpp',
  include_directories: include_directories('src'),
  link_args: ['-static', '-static-libgcc', '-static-libstdc++'])
test('Game window ownership across redirected device creation', device_window_test)

source_lights_test = executable('source-lights-test', 'tests/source_lights.cpp', 'src/source_lights.cpp',
  include_directories: include_directories('src', remix_inc),
  link_args: ['-static', '-static-libgcc', '-static-libstdc++'])
test('Source emitter intensity, angular falloff and ABI', source_lights_test)
styled_lights_test = executable('styled-lights-test', 'tests/styled_lights.cpp',
  'src/source_lights.cpp', 'src/styled_lights.cpp',
  include_directories: include_directories('src', remix_inc),
  link_args: ['-static', '-static-libgcc', '-static-libstdc++'])
test('Styled light immutable capture and live identity', styled_lights_test)
