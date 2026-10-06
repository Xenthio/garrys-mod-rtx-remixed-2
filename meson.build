project('gmodrtx', 'cpp', version: '0.1.0',
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

shared_library('shaderapidx9',
  'src/main.cpp', 'src/vtable_proxy.cpp', 'src/d3d_hooks.cpp', 'src/remix_output.cpp', 'src/texture_formats.cpp',
  'src/dxvk_hash.cpp', 'src/texture_categories.cpp', 'src/source_fs.cpp', 'src/bsp_world.cpp', 'src/engine_hooks.cpp',
  'src/remix_ui.cpp', 'src/engine_patches.cpp', 'src/engine_lights.cpp', 'src/engine_shadows.cpp',
  include_directories: include_directories(remix_inc, sdl_inc, 'deps/xxhash'),
  name_prefix: '',
  vs_module_defs: 'shaderapidx9.def',
  link_args: ['-static', '-static-libgcc', '-static-libstdc++'],
)
