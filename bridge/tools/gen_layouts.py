#!/usr/bin/env python3
"""Generate x86/x64 struct layout tables for the Remix API bridge.

Parses the OpenRemix public headers twice with libclang (i686 and x86_64
MinGW targets) and emits `layouts.gen.h`: for every struct, its size and
alignment on both ABIs and a field list describing how the 32-bit client
re-encodes it in the 64-bit layout the host hands to the runtime.

Structs declared in remix_c.h and remix_native_ext.h are emitted.

Usage: gen_layouts.py --include <openremix>/public/include --out <file> [--check]
"""
import argparse
import re
import subprocess
import sys

import clang.cindex as ci

HEADERS = ('remix_c.h', 'remix_native_ext.h')

# Pointer fields that do not follow the `X_values` / `X_count` convention.
# (struct, field) -> (kind, size field or element-count field)
ARRAYS = {
    ('remixapi_UIDrawList', 'pVertices'): 'vertexCount',
    ('remixapi_UIDrawList', 'pIndices'): 'indexCount',
    ('remixapi_UIDrawList', 'pCommands'): 'commandCount',
    ('remixapi_AnimatedFloat1D', 'pData'): 'numberElements',
    ('remixapi_AnimatedFloat2D', 'pData'): 'numberElements',
    ('remixapi_AnimatedFloat3D', 'pData'): 'numberElements',
    ('remixapi_AnimatedFloat4D', 'pData'): 'numberElements',
    ('remixapi_CameraScreensEXT', 'cameras'): 'count',
    ('remixapi_HostShaderInfoEXT', 'bindings'): 'bindingCount',
    ('remixapi_HostPassListEXT', 'passes'): 'passCount',
    ('remixapi_HostPassEXT', 'buffers'): 'bufferCount',
    ('remixapi_HostPassEXT', 'samplers'): 'samplerCount',
    ('remixapi_HostPassEXT', 'vertexBindings'): 'vertexBindingCount',
    ('remixapi_HostPassEXT', 'attributes'): 'attributeCount',
    ('remixapi_HostPassEXT', 'indices'): 'indexCount',
    ('remixapi_HostPassListFlagsEXT', 'flags'): 'flagCount',
}
# Raw bytes; checked before the char* -> CSTR rule (an IES profile is not
# NUL-terminated).
BLOBS = {
    ('remixapi_TextureInfo', 'data'): 'dataSize',
    ('remixapi_HostShaderInfoEXT', 'code'): 'codeBytes',
    ('remixapi_HostPassEXT', 'pushConstants'): 'pushConstantBytes',
    ('remixapi_HostPassBufferEXT', 'data'): 'bytes',
    ('remixapi_HostPassVertexBindingEXT', 'data'): 'bytes',
    ('remixapi_LightInfoIesProfileEXT', 'data'): 'size',
}
# Pointers to a single optional value.
OPTIONAL_PREFIX = {'remixapi_LightInfoUSDEXT'}
# Pointers the bridge cannot carry across processes; they arrive as null.
UNSUPPORTED = {
    # Carried by its own op (Op::SetVisibilityPartition, inline arrays).
    ('remixapi_VisibilityPartitionEXT', 'planes'),
    ('remixapi_VisibilityPartitionEXT', 'children'),
    ('remixapi_VisibilityPartitionEXT', 'leafClusters'),
    ('remixapi_VisibilityPartitionEXT', 'visibility'),
}
# Structs left out of the tables: never encoded field by field (the bridge
# copies them as raw bytes; identical on both ABIs).
EXCLUDED = {'remixapi_VramStats'}
HANDLES = {
    'remixapi_MaterialHandle': 'HANDLE_MATERIAL',
    'remixapi_MeshHandle': 'HANDLE_MESH',
}
# Integer handles (uint64_t typedefs) the client assigns as ids and the host
# translates.
VALUE_HANDLES = {
    'remixapi_HostShaderHandleEXT': 'HANDLE_HOST_SHADER',
}


def resource_dir():
    return subprocess.check_output(['clang', '-print-resource-dir'], text=True).strip()


def parse(include, triple, sysroot):
    src = '#define REMIX_ALLOW_X86\n' + ''.join(f'#include <remix/{h}>\n' for h in HEADERS)
    args = ['-target', triple, '-I' + include, '-isystem', sysroot + '/include',
            '-isystem', resource_dir() + '/include', '-x', 'c', '-std=c11']
    tu = ci.Index.create().parse('bridge_layouts.c', args=args,
                                 unsaved_files=[('bridge_layouts.c', src)],
                                 options=ci.TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD)
    errors = [d for d in tu.diagnostics if d.severity >= ci.Diagnostic.Error]
    if errors:
        sys.exit('\n'.join(str(d) for d in errors))
    return tu


def owned(cursor):
    f = cursor.location.file
    return f is not None and f.name.replace('\\', '/').split('/')[-1] in HEADERS


def collect_structs(tu):
    out = {}
    for c in tu.cursor.walk_preorder():
        if c.kind == ci.CursorKind.STRUCT_DECL and c.is_definition() and owned(c):
            name = c.spelling or c.type.spelling
            if name.startswith('struct '):
                name = name[7:]
            if name.startswith('remixapi_') and name != 'remixapi_Interface' and name not in EXCLUDED:
                out[name] = c.type
    return out


def collect_stypes(tu):
    values = {}
    for c in tu.cursor.walk_preorder():
        if c.kind == ci.CursorKind.ENUM_CONSTANT_DECL and c.spelling.startswith('REMIXAPI_STRUCT_TYPE_'):
            values[c.spelling] = c.enum_value
        elif c.kind == ci.CursorKind.MACRO_DEFINITION and c.spelling.startswith('REMIXAPI_STRUCT_TYPE_'):
            text = ' '.join(t.spelling for t in c.get_tokens())
            m = re.search(r'0x[0-9a-fA-F]+', text)
            if m:
                values[c.spelling] = int(m.group(0), 16)
    return values


def snake(name):
    base = name[len('remixapi_'):]
    base = re.sub(r'EXT$', 'Ext', base)
    s = re.sub(r'(?<=[a-z0-9])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])', '_', base).upper()
    return 'REMIXAPI_STRUCT_TYPE_' + s


STYPE_ALIASES = {
    'remixapi_LightInfoSphereEXT': 'REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT',
    'remixapi_UIDrawList': 'REMIXAPI_STRUCT_TYPE_UI_DRAW_LIST',
    'remixapi_LightInfoUSDEXT': 'REMIXAPI_STRUCT_TYPE_LIGHT_INFO_USD_EXT',
    'remixapi_MaterialInfoPbrEXT': 'REMIXAPI_STRUCT_TYPE_MATERIAL_PBR_EXT',
    'remixapi_MaterialInfoEmissionEXT': 'REMIXAPI_STRUCT_TYPE_MATERIAL_EMISSION_EXT',
    'remixapi_MeshInfoUpdateEXT': 'REMIXAPI_STRUCT_TYPE_MESH_UPDATE_EXT',
    'remixapi_MeshInfoMotionEXT': 'REMIXAPI_STRUCT_TYPE_MESH_MOTION_EXT',
    'remixapi_InstanceRasterStateEXT': 'REMIXAPI_STRUCT_TYPE_INSTANCE_RASTER_STATE_EXT',
    'remixapi_InstanceIdentityEXT': 'REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT',
    'remixapi_CameraTemporalEXT': 'REMIXAPI_STRUCT_TYPE_CAMERA_TEMPORAL_EXT',
    'remixapi_CameraEnvironmentEXT': 'REMIXAPI_STRUCT_TYPE_CAMERA_ENVIRONMENT_EXT',
    'remixapi_MaterialNormalEncodingEXT': 'REMIXAPI_STRUCT_TYPE_MATERIAL_NORMAL_ENCODING_EXT',
    'remixapi_TextureInfoGenerateMipsEXT': 'REMIXAPI_STRUCT_TYPE_TEXTURE_GENERATE_MIPS_EXT',
    'remixapi_MaterialInfoSelfIlluminationEXT': 'REMIXAPI_STRUCT_TYPE_MATERIAL_SELF_ILLUMINATION_EXT',
    'remixapi_TextureInfoDynamicEXT': 'REMIXAPI_STRUCT_TYPE_TEXTURE_DYNAMIC_EXT',
    'remixapi_MeshInfoSpriteVerticesEXT': 'REMIXAPI_STRUCT_TYPE_MESH_SPRITE_VERTICES_EXT',
    'remixapi_MeshInfoDeformEXT': 'REMIXAPI_STRUCT_TYPE_MESH_DEFORM_EXT',
}


def stype_of(name, fields, stypes):
    if not fields or fields[0][0] != 'sType':
        return None
    key = STYPE_ALIASES.get(name, snake(name))
    if key not in stypes:
        # A wrong derivation would silently encode sType 0 (dropped from
        # every pNext chain): add the struct to STYPE_ALIASES.
        sys.exit(f'no sType constant {key} for {name}: add it to STYPE_ALIASES')
    return key


def check_annotations(structs):
    """Fails on annotations that name structs or fields the headers lack."""
    stale = []
    for table in (ARRAYS, BLOBS):
        for (n, fn), count in table.items():
            fields = [f.spelling for f in structs[n].get_fields()] if n in structs else []
            if fn not in fields or count not in fields:
                stale.append(f'{n}.{fn} ({count})')
    for n, fn in UNSUPPORTED:
        if n not in structs or fn not in [f.spelling for f in structs[n].get_fields()]:
            stale.append(f'{n}.{fn}')
    stale += [n for n in list(STYPE_ALIASES) + list(OPTIONAL_PREFIX) if n not in structs]
    if stale:
        sys.exit('stale annotations: ' + ', '.join(stale))


def pointee_record(t):
    p = t.get_canonical().get_pointee().get_canonical()
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--include', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()

    tu32 = parse(a.include, 'i686-w64-windows-gnu', '/usr/i686-w64-mingw32')
    tu64 = parse(a.include, 'x86_64-w64-windows-gnu', '/usr/x86_64-w64-mingw32')
    s32, s64 = collect_structs(tu32), collect_structs(tu64)
    stypes = collect_stypes(tu64)
    names = sorted(s64)
    index = {n: i for i, n in enumerate(names)}
    check_annotations(s64)

    def field_list(t):
        return [(f.spelling, f) for f in t.get_fields()]

    def has_pointer(t, seen=()):
        for _, f in field_list(t):
            k = f.type.get_canonical()
            if k.kind == ci.TypeKind.POINTER:
                return True
            if k.kind == ci.TypeKind.RECORD and has_pointer(k, seen):
                return True
        return False

    def same_layout(name):
        t32, t64 = s32[name], s64[name]
        if t32.get_size() != t64.get_size():
            return False
        return all(f32.get_field_offsetof() == f64.get_field_offsetof()
                   for (_, f32), (_, f64) in zip(field_list(t32), field_list(t64)))

    def record_name(t):
        return re.sub(r'\b(const|volatile|struct)\s+', '', t.get_canonical().spelling).strip()

    lines = []
    emit = lines.append
    emit('// Generated by tools/gen_layouts.py from the OpenRemix public headers. Do not edit.')
    emit('#pragma once')
    emit('#include "layout.h"')
    emit('')
    emit('namespace bridge::layout {')
    emit('')
    emit('enum StructId : uint16_t {')
    for n in names:
        emit(f'  S_{n[9:]} = {index[n]},')
    emit(f'  S_COUNT = {len(names)},')
    emit('};')
    emit('')

    struct_rows = []
    for n in names:
        t32, t64 = s32[n], s64[n]
        f32 = dict(field_list(t32))
        fields = field_list(t64)
        fnames = [fn for fn, _ in fields]
        rows = []
        for fi, (fn, f) in enumerate(fields):
            o32 = f32[fn].get_field_offsetof() // 8
            o64 = f.get_field_offsetof() // 8
            ct = f.type.get_canonical()
            spelled = f.type.spelling.replace('const ', '')
            size64 = f.type.get_size()
            kind, aux, link = 'POD', 0, 0xFFFF
            if ct.kind == ci.TypeKind.POINTER:
                pt = pointee_record(f.type)
                if fn == 'pNext':
                    kind = 'PNEXT'
                elif spelled in HANDLES:
                    kind = HANDLES[spelled]
                elif spelled == 'remixapi_HWND':
                    kind = 'HWND'
                elif spelled == 'remixapi_Path':
                    kind = 'WSTR'
                elif (n, fn) in BLOBS:
                    kind, link = 'BLOB', fnames.index(BLOBS[(n, fn)])
                elif (n, fn) in UNSUPPORTED or pt.kind in (ci.TypeKind.FUNCTIONPROTO, ci.TypeKind.VOID):
                    kind = 'NULLPTR'
                elif pt.kind in (ci.TypeKind.CHAR_S, ci.TypeKind.CHAR_U):
                    kind = 'CSTR'
                else:
                    count = ARRAYS.get((n, fn))
                    if count is None and fn.endswith('_values'):
                        count = fn[:-len('_values')] + '_count'
                    if count is None and n in OPTIONAL_PREFIX:
                        kind = 'OPTIONAL'
                    elif count is None or count not in fnames:
                        sys.exit(f'unannotated pointer {n}.{fn} ({f.type.spelling})')
                    else:
                        kind, link = 'ARRAY', fnames.index(count)
                    if pt.kind == ci.TypeKind.RECORD:
                        rn = record_name(pt)
                        if has_pointer(s64[rn]) or not same_layout(rn):
                            kind = 'ARRAY_STRUCT' if kind == 'ARRAY' else sys.exit(f'optional struct {n}.{fn}')
                            aux = index[rn]
                        else:
                            aux = pt.get_size()
                    else:
                        aux = pt.get_size()
            elif spelled in VALUE_HANDLES:
                kind = VALUE_HANDLES[spelled]
            elif ct.kind == ci.TypeKind.RECORD:
                rn = record_name(ct)
                if has_pointer(s64[rn]) or not same_layout(rn):
                    kind, aux = 'NESTED', index[rn]
            elif ct.kind == ci.TypeKind.CONSTANTARRAY:
                et = ct.get_array_element_type().get_canonical()
                if et.kind == ci.TypeKind.RECORD and has_pointer(s64[record_name(et)]):
                    sys.exit(f'array of pointer structs {n}.{fn}')
            if kind == 'POD' and f32[fn].type.get_size() != size64:
                sys.exit(f'size mismatch {n}.{fn}')
            rows.append(f'    {{{o32}, {o64}, {size64}, F_{kind}, {aux}, {link}}}, // {f.type.spelling} {fn}')
        key = stype_of(n, fields, stypes)
        stype = stypes[key] if key else 0
        emit(f'inline constexpr FieldDesc k{n[9:]}Fields[] = {{')
        lines.extend(rows if rows else ['    {0, 0, 0, F_POD, 0, 0xFFFF},'])
        emit('};')
        struct_rows.append(f'  {{"{n}", {stype:#x}u, {t32.get_size()}, {t64.get_size()}, '
                           f'{t32.get_align()}, {t64.get_align()}, {len(rows)}, k{n[9:]}Fields}},')
    emit('')
    emit('inline constexpr StructDesc kStructs[S_COUNT] = {')
    lines.extend(struct_rows)
    emit('};')
    emit('')
    emit('} // namespace bridge::layout')
    text = '\n'.join(lines) + '\n'

    if a.check:
        try:
            current = open(a.out).read()
        except FileNotFoundError:
            current = ''
        if current != text:
            sys.exit(f'{a.out} is out of date; rerun tools/gen_layouts.py')
        return
    with open(a.out, 'w') as fp:
        fp.write(text)


if __name__ == '__main__':
    main()
