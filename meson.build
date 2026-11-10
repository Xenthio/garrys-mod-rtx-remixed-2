project('gmodrtx', ['cpp', 'c'], version: '0.1.0',
        default_options: ['cpp_std=c++17', 'warning_level=2', 'buildtype=release', 'b_ndebug=if-release'])

# One tree, six games, chosen at configure time (-Dgame=, default by the
# cross file): Garry's Mod x64 or Team Fortress 2 x64
# (cross/x86_64-w64-mingw32.txt, -Dgame=tf2), Left 4 Dead 2 x86, Half-Life 2
# RTX x86 (-Dgame=hl2rtx), Entropy: Zero 2 x86 (-Dgame=ez2) or Portal 2 x86
# (-Dgame=portal2) (cross/i686-w64-mingw32.txt). src/game.h holds what
# differs.
game = get_option('game')
cpu = host_machine.cpu_family()
if game == 'auto'
  game = cpu == 'x86_64' ? 'gmod' : 'l4d2'
endif
if game == 'gmod' and cpu != 'x86_64'
  error('the Garry\'s Mod proxy is a 64-bit DLL: configure with cross/x86_64-w64-mingw32.txt')
endif
if game == 'tf2' and cpu != 'x86_64'
  error('the Team Fortress 2 proxy is a 64-bit DLL (tf_win64.exe): configure with cross/x86_64-w64-mingw32.txt')
endif
if game == 'l4d2' and cpu != 'x86'
  error('the Left 4 Dead 2 proxy is a 32-bit DLL: configure with cross/i686-w64-mingw32.txt')
endif
if game == 'hl2rtx' and cpu != 'x86'
  error('the Half-Life 2 RTX proxy is a 32-bit DLL: configure with cross/i686-w64-mingw32.txt')
endif
if game == 'ez2' and cpu != 'x86'
  error('the Entropy: Zero 2 proxy is a 32-bit DLL: configure with cross/i686-w64-mingw32.txt')
endif
if game == 'portal2' and cpu != 'x86'
  error('the Portal 2 proxy is a 32-bit DLL: configure with cross/i686-w64-mingw32.txt')
endif

remix_inc = get_option('openremix_include')
if remix_inc == ''
  error('set -Dopenremix_include=<openremix>/public/include')
endif
# SDL3 headers for the panel's events (the runtime ships SDL3_shared.dll).
sdl_inc = get_option('sdl_include')
if sdl_inc == ''
  sdl_inc = remix_inc / '..' / '..' / 'subprojects' / 'SDL3-3.4.14' / 'include'
endif

subdir('deps/dxso')
bcrypt_dep = meson.get_compiler('cpp').find_library('bcrypt')

# The proxy's own host-pass shaders (src/shaders), SPIR-V as C arrays.
glslang = find_program('glslangValidator')
host_shaders = []
foreach shader : ['host_depth.vert', 'host_depth.frag', 'host_frame_depth.frag']
  host_shaders += custom_target(shader + '.inc',
    input: 'src/shaders' / shader, output: shader + '.inc',
    command: [glslang, '-V', '--vn', shader.replace('.', '_'), '-o', '@OUTPUT@', '@INPUT@'])
endforeach

sources = [
  'src/main.cpp', 'src/vtable_proxy.cpp', 'src/d3d_hooks.cpp', 'src/remix_output.cpp', 'src/texture_formats.cpp',
  'src/dxvk_hash.cpp', 'src/texture_categories.cpp', 'src/source_fs.cpp', 'src/bsp_world.cpp',
  'src/refract_material.cpp', 'src/refract_native.cpp', 'src/cubemap_overrides.cpp',
  'src/engine_materials.cpp', 'src/material_shading.cpp', 'src/pbr_rules.cpp',
  'src/vertex_program.cpp', 'src/particles.cpp', 'src/diag.cpp', 'src/lzma_source.cpp',
  'src/host_passes.cpp', 'src/vb_origins.cpp', 'src/bake_sky.cpp', 'src/bake_sky_cache.cpp',
  'src/source_lights.cpp', 'src/styled_lights.cpp', 'src/frame_profile.cpp',
  'src/shader_tables.cpp', 'src/fixed_function.cpp',
  'deps/lzma/LzmaDec.c', host_shaders,
]
cpp_compiler = meson.get_compiler('cpp')
is_gcc_syntax = cpp_compiler.get_argument_syntax() == 'gcc'
cpp_args = ['-DNOMINMAX', '-D_CRT_SECURE_NO_WARNINGS']
link_args = []
if is_gcc_syntax
  link_args += ['-static', '-static-libgcc', '-static-libstdc++']
endif
if game == 'gmod'
  cpp_args += '-DPROXY_GAME_GMOD'
  sources += ['src/engine_hooks.cpp', 'src/remix_ui.cpp', 'src/engine_patches.cpp', 'src/engine_lights.cpp',
              'src/engine_shadows.cpp', 'src/engine_nocull.cpp', 'src/engine_mirrors.cpp',
              'src/workshop_content.cpp']
  def_file = 'shaderapidx9.def'
elif game == 'tf2'
  # Team Fortress 2 x64: OpenRemix in process and GMod's interface-based engine
  # hooks and panel; GMod's build-specific engine patches off (their entry
  # points as no-ops); the module helpers for the classes it finds by RTTI.
  cpp_args += '-DPROXY_GAME_TF2'
  sources += ['src/engine_hooks.cpp', 'src/remix_ui.cpp', 'src/gmod_only_stubs.cpp', 'src/pe_module.cpp']
  def_file = 'shaderapidx9.def'
else
  # x86: the Remix API is used through the bridge (remix_c.h is 64-bit only
  # unless allowed; empty, as the headers that define it themselves). The
  # GMod-only modules' entry points as no-ops, the 32-bit module helpers and
  # the runtime UI in the bridge host.
  cpp_args += ['-DREMIX_ALLOW_X86=']
  sources += ['src/gmod_only_stubs.cpp', 'src/pe_module.cpp', 'src/bridge_ui.cpp']
  if is_gcc_syntax
    link_args += '-Wl,--enable-stdcall-fixup'
  endif
  if game == 'l4d2'
    # The x86 engine side (src/x86_engine.h) and L4D2's own (src/l4d2.h).
    cpp_args += '-DPROXY_GAME_L4D2'
    sources += ['src/x86_engine.cpp', 'src/x86_cull.cpp', 'src/l4d2_lights.cpp', 'src/l4d2_infected.cpp']
    def_file = 'shaderapidx9_l4d2.def'
  elif game == 'portal2'
    # Portal 2: the x86 engine side with Portal 2's patterns (src/x86_engine.h)
    # and GMod's projected textures (src/engine_lights.h: env_projectedtexture),
    # the portals as ray portals (src/portal2.h).
    cpp_args += '-DPROXY_GAME_PORTAL2'
    sources += ['src/x86_engine.cpp', 'src/x86_cull.cpp', 'src/engine_lights.cpp', 'src/portal2_portals.cpp']
    def_file = 'shaderapidx9_portal2.def'
  elif game == 'hl2rtx'
    # Half-Life 2 RTX: no engine side yet (src/hl2rtx_engine.cpp).
    cpp_args += '-DPROXY_GAME_HL2RTX'
    sources += ['src/hl2rtx_engine.cpp']
    def_file = 'shaderapidx9_hl2rtx.def'
  elif game == 'ez2'
    # Entropy : Zero 2 (src/ez2_engine.cpp).
    cpp_args += '-DPROXY_GAME_EZ2'
    sources += ['src/ez2_engine.cpp']
    def_file = 'shaderapidx9_hl2rtx.def'
  endif
endif

shared_library('shaderapidx9', sources,
  include_directories: include_directories(remix_inc, sdl_inc, 'deps/xxhash'),
  dependencies: [dxso_dep, bcrypt_dep],
  cpp_args: cpp_args,
  name_prefix: '',
  vs_module_defs: def_file,
  link_args: link_args,
)

# The branch's BSP replay and host fixtures target the GMod x64 installation.
if game == 'gmod'
material_parameters_test = executable('material-parameters-test', 'tests/material_parameters.cpp',
  include_directories: include_directories('src'),
  link_args: ['-static', '-static-libgcc', '-static-libstdc++'])
test('Material name queries preserve names, misses and layout replacement', material_parameters_test)

bake_charts_test = executable('bsp-bake-charts-test', 'tests/bake_charts.cpp',
  include_directories: include_directories('src'),
  link_args: ['-static', '-static-libgcc', '-static-libstdc++'])
test('BSP surface lightmap chart mapping', bake_charts_test)

refract_material_test = executable('refract-material-test', 'tests/refract_material.cpp', 'src/refract_material.cpp',
  'src/refract_native.cpp',
  include_directories: include_directories('src', remix_inc),
  link_args: ['-static', '-static-libgcc', '-static-libstdc++'])
test('Source Refract proxy binding and passive optical tint', refract_material_test)

bake_materials_test = executable('bsp-bake-materials-test', 'tests/bake_materials.cpp',
  include_directories: include_directories('src'),
  link_args: ['-static', '-static-libgcc', '-static-libstdc++'])
test('BSP bake readiness follows retained material dependencies', bake_materials_test)

source_fs_test = executable('source-fs-test', 'tests/source_fs.cpp', 'src/source_fs.cpp',
  'src/lzma_source.cpp', 'deps/lzma/LzmaDec.c',
  cpp_args: cpp_args,
  include_directories: include_directories('src'),
  link_args: ['-static', '-static-libgcc', '-static-libstdc++'])
test('Source loose and VPK relative model paths', source_fs_test,
  args: [meson.current_build_dir() / 'source-fs-readback'])
test('Source default content roots without optional game mounts', source_fs_test,
  args: [meson.current_build_dir() / 'source-fs-default-readback', 'without-mounts'])

# A diagnostic map host: reads installed content without injecting into the game.
executable('bsp-replay', 'tools/bsp_replay.cpp', 'tools/replay_settings.cpp', 'tools/replay_timing.cpp', 'tools/probe_receivers.cpp',
  'tools/replay_sky.cpp', 'src/bake_sky_cache.cpp',
  'src/bsp_world.cpp', 'src/refract_material.cpp', 'src/refract_native.cpp', 'src/cubemap_overrides.cpp', 'src/source_fs.cpp', 'src/lzma_source.cpp',
  'src/texture_formats.cpp', 'src/texture_categories.cpp', 'src/pbr_rules.cpp',
  'src/material_shading.cpp', 'src/source_lights.cpp', 'src/styled_lights.cpp', 'deps/lzma/LzmaDec.c',
  include_directories: include_directories('src', remix_inc, 'deps/xxhash'),
  cpp_args: cpp_args,
  dependencies: [meson.get_compiler('cpp').find_library('user32'), bcrypt_dep],
  link_args: ['-static', '-static-libgcc', '-static-libstdc++'])

replay_sky_test = executable('replay-sky-test', 'tests/replay_sky.cpp', 'tools/replay_sky.cpp', 'src/bake_sky_cache.cpp',
  include_directories: include_directories('tools', 'src', remix_inc, 'deps/xxhash'),
  link_args: ['-static', '-static-libgcc', '-static-libstdc++'])
test('Replay sky dome orientation and complete face validation', replay_sky_test)

replay_settings_test = executable('replay-settings-test', 'tests/replay_settings.cpp', 'tools/replay_settings.cpp',
  include_directories: include_directories('tools', remix_inc),
  link_args: ['-static', '-static-libgcc', '-static-libstdc++'])
test('Replay camera settings and Source orientation', replay_settings_test)
replay_timing_test = executable('replay-timing-test', 'tests/replay_timing.cpp', 'tools/replay_timing.cpp',
  include_directories: include_directories('tools', remix_inc),
  link_args: ['-static', '-static-libgcc', '-static-libstdc++'])
test('Replay steady-state GPU timing window', replay_timing_test)
replay_report_test = executable('replay-report-test', 'tests/replay_report.cpp',
  include_directories: include_directories('tools'),
  link_args: ['-static', '-static-libgcc', '-static-libstdc++'])
test('Replay diagnostics require a fresh nonempty response', replay_report_test)

shader_bytecode_test = executable('shader-bytecode-test', 'tests/shader_bytecode.cpp',
  include_directories: include_directories('src'),
  link_args: ['-static', '-static-libgcc', '-static-libstdc++'])
test('Bounded D3D9 shader bytecode capture', shader_bytecode_test)

d3d_method_test = executable('d3d-method-test', 'tests/d3d_method.cpp',
  include_directories: include_directories('src'),
  link_args: ['-static', '-static-libgcc', '-static-libstdc++'])
test('D3D9 resource methods across distinct vtables', d3d_method_test)

diagnostic_module = shared_library('diagnostic-module-fixture', 'tests/diagnostic_module.cpp',
  include_directories: include_directories('src'),
  link_args: ['-static-libgcc', '-static-libstdc++'])
diagnostic_workers_test = executable('diagnostic-workers-test', 'tests/diagnostic_workers.cpp',
  include_directories: include_directories('src'),
  link_args: ['-static', '-static-libgcc', '-static-libstdc++'])
test('Diagnostic workers stop before shutdown and retain hook code across host release',
  diagnostic_workers_test, args: [diagnostic_module.full_path()], depends: diagnostic_module, timeout: 15)

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

remix_ui_test = executable('remix-ui-test', 'tests/remix_ui.cpp', 'src/remix_ui.cpp',
  include_directories: include_directories('src', remix_inc, sdl_inc),
  dependencies: meson.get_compiler('cpp').find_library('user32'),
  cpp_args: cpp_args,
  link_args: ['-static', '-static-libgcc', '-static-libstdc++'])
sdl_test_dll = get_option('sdl_test_dll')
if sdl_test_dll != ''
  test('Runtime shortcut packaged SDL input bridge and native window ownership', remix_ui_test,
    args: [sdl_test_dll])
endif

source_lights_test = executable('source-lights-test', 'tests/source_lights.cpp', 'src/source_lights.cpp',
  include_directories: include_directories('src', remix_inc),
  link_args: ['-static', '-static-libgcc', '-static-libstdc++'])
test('Source emitter intensity, angular falloff and ABI', source_lights_test)
styled_lights_test = executable('styled-lights-test', 'tests/styled_lights.cpp',
  'src/source_lights.cpp', 'src/styled_lights.cpp',
  include_directories: include_directories('src', remix_inc),
  link_args: ['-static', '-static-libgcc', '-static-libstdc++'])
test('Styled light immutable capture and live identity', styled_lights_test)

cubemap_overrides_test = executable('cubemap-overrides-test', 'tests/cubemap_overrides.cpp', 'src/cubemap_overrides.cpp',
  include_directories: include_directories('src', remix_inc), dependencies: bcrypt_dep,
  link_args: ['-static', '-static-libgcc', '-static-libstdc++'])
test('Cubemap authoring sidecar fingerprint and live publication', cubemap_overrides_test,
  args: [meson.current_build_dir() / 'cubemap-overrides-readback'])

cubemap_receivers_test = executable('cubemap-receivers-test', 'tests/cubemap_receivers.cpp',
  include_directories: include_directories('src', remix_inc),
  link_args: ['-static', '-static-libgcc', '-static-libstdc++'])
test('Explicit BSP material capture names and authored identity matching', cubemap_receivers_test)

sky_mesh_clip_test = executable('sky-mesh-clip-test', 'tests/sky_mesh_clip.cpp',
  include_directories: include_directories('src', remix_inc),
  link_args: ['-static', '-static-libgcc', '-static-libstdc++'])
test('Placed sky meshes preserve clipped geometry and replacement coordinates', sky_mesh_clip_test)

endif # GMod-only development fixtures
