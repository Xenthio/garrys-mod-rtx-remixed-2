project('gmodrtx', 'cpp', version: '0.1.0',
        default_options: ['cpp_std=c++17', 'warning_level=2', 'buildtype=release', 'b_ndebug=if-release'])

# 64-bit only (Garry's Mod x86-64 branch): configure with cross/x86_64-w64-mingw32.txt.
if host_machine.cpu_family() != 'x86_64'
  error('the shaderapidx9 proxy targets Garry\'s Mod x64')
endif

shared_library('shaderapidx9',
  'src/main.cpp', 'src/vtable_proxy.cpp',
  name_prefix: '',
  vs_module_defs: 'shaderapidx9.def',
  link_args: ['-static', '-static-libgcc', '-static-libstdc++'],
)
