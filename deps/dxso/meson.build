# DXVK's D3D9 shader compiler (dxso, zlib licence: LICENSE) standing alone:
# D3D9 vertex/pixel shader bytecode to SPIR-V, with DXVK's own util and
# SPIR-V builder, and shims in src/d3d9, src/dxvk and src/dxbc in place of the
# DXVK internals it was written against (see README.md).
dxso_inc = include_directories('src', 'include')
dxso_src = files(
  'src/dxso/dxso_analysis.cpp', 'src/dxso/dxso_code.cpp', 'src/dxso/dxso_common.cpp', 'src/dxso/dxso_compiler.cpp',
  'src/dxso/dxso_ctab.cpp', 'src/dxso/dxso_decoder.cpp', 'src/dxso/dxso_enums.cpp', 'src/dxso/dxso_header.cpp',
  'src/dxso/dxso_module.cpp', 'src/dxso/dxso_options.cpp', 'src/dxso/dxso_reader.cpp', 'src/dxso/dxso_tables.cpp',
  'src/dxso/dxso_util.cpp',
  'src/spirv/spirv_code_buffer.cpp', 'src/spirv/spirv_compression.cpp', 'src/spirv/spirv_module.cpp',
  'src/d3d9/d3d9_fixed_function.cpp',
  'src/util/log/log.cpp', 'src/util/util_string.cpp', 'src/util/util_env.cpp', 'src/util/dxso_standalone.cpp',
)
dxso_vulkan_inc = include_directories(get_option('openremix_include') / '..' / '..' / 'include' / 'vulkan' / 'include')
dxso_lib = static_library('dxso', dxso_src,
  include_directories: [dxso_inc, dxso_vulkan_inc],
  cpp_args: ['-w'],
)
dxso_dep = declare_dependency(link_with: dxso_lib, include_directories: [dxso_inc, dxso_vulkan_inc])
