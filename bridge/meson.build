project('openremix-bridge', 'cpp', version: '0.1.0',
        default_options: ['cpp_std=c++17', 'warning_level=2', 'buildtype=release', 'b_ndebug=if-release'])

# Configure twice: cross/i686-w64-mingw32.txt builds the client DLL (and test
# app) that loads into 32-bit games, cross/x86_64-w64-mingw32.txt builds the
# host executable that runs the OpenRemix runtime.
remix_inc = get_option('openremix_include')
if remix_inc == ''
  error('set -Dopenremix_include=<openremix>/public/include')
endif
sdl_inc = get_option('sdl_include')
if sdl_inc == ''
  sdl_inc = remix_inc / '..' / '..' / 'subprojects' / 'SDL3-3.4.14' / 'include'
endif
# The OpenRemix headers are system headers here: their inline loader's casts
# would warn at warning_level=2.
inc = [include_directories('src'), include_directories(remix_inc, is_system: true), include_directories(sdl_inc, is_system: true)]
cpp_compiler = meson.get_compiler('cpp')
is_gcc_syntax = cpp_compiler.get_argument_syntax() == 'gcc'
link_static = []
if is_gcc_syntax
  link_static = ['-static', '-static-libgcc', '-static-libstdc++']
endif
cpu = host_machine.cpu_family()
sample_inc = include_directories(remix_inc / '..' / '..' / 'samples' / 'remix-native', is_system: true)

if cpu == 'x86'
  client_link_args = link_static
  client_kw = {}
  if is_gcc_syntax
    client_link_args += ['-Wl,--kill-at']
  else
    # MSVC-syntax toolchains (clang-cl) export the stdcall names through a .def
    client_kw = {'vs_module_defs': 'remix_bridge.def'}
  endif

  client = shared_library('remix_bridge',
    'src/client/api.cpp', 'src/client/encoder.cpp', 'src/client/transport.cpp',
    include_directories: inc,
    name_prefix: '',
    cpp_args: ['-DNOMINMAX', '-D_CRT_SECURE_NO_WARNINGS'],
    link_args: client_link_args,
    kwargs: client_kw,
  )
  if is_gcc_syntax
    executable('bridge_smoke_x86', 'tests/smoke.cpp',
      include_directories: inc + [sample_inc],
      link_args: link_static,
    )
  endif
elif cpu == 'x86_64'
  host_cpp_args = ['-DNOMINMAX', '-D_CRT_SECURE_NO_WARNINGS']
  host_link_args = link_static
  if is_gcc_syntax
    host_cpp_args += ['-municode']
    host_link_args += ['-municode', '-mwindows']
  endif

  executable('openremix_bridge_host', 'src/host/main.cpp', 'src/host/profiler.cpp', 'src/host/input.cpp',
    include_directories: inc + [include_directories(get_option('sdl_include'), is_system: true)],
    cpp_args: host_cpp_args,
    link_args: host_link_args,
    win_subsystem: 'windows',
  )
  # Reference: the same smoke test calling remix.dll directly.
  if is_gcc_syntax
    executable('bridge_smoke_x64', 'tests/smoke.cpp',
      include_directories: inc + [sample_inc],
      link_args: link_static,
    )
  endif
else
  error('unsupported cpu family ' + cpu)
endif
