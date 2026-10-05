#!/usr/bin/env python3
"""Emulate MSVC x64 vtable slot order for Source SDK interfaces (GMod: sourcesdk-minimal).

Parses SDK headers with libclang (declaration order) and applies the MSVC
rules: a class's new virtuals follow its primary base's slots; overloads of one
name are grouped at the first declaration, in reverse declaration order; a
virtual destructor takes one slot. Overrides of inherited virtuals take no slot.

Usage: sdk_vtables.py --sdk <hl2sdk> Class [Class...] [--header h ...] [--cpp OUT.h]
"""
import argparse
import os
import subprocess
import tempfile
import sys

import clang.cindex as ci


def lowercase_shadow(sdk):
    """The SDK assumes a case-insensitive file system: mirror every header
    under a lowercase path in a temporary tree searched after the real one."""
    root = os.path.join(tempfile.gettempdir(), 'sdk_vtables_lc_' + str(abs(hash(sdk))))
    if os.path.isdir(root):
        return root
    for dirpath, _, files in os.walk(sdk):
        for f in files:
            if not f.lower().endswith(('.h', '.inl')):
                continue
            rel = os.path.relpath(os.path.join(dirpath, f), sdk).lower()
            dst = os.path.join(root, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if not os.path.exists(dst):
                os.symlink(os.path.join(dirpath, f), dst)
    return root


def parse(sdk, headers):
    rd = subprocess.check_output(['clang', '-print-resource-dir'], text=True).strip()
    lc = lowercase_shadow(sdk)
    src = ''.join(f'#include "{h}"\n' for h in headers)
    args = ['-target', 'x86_64-w64-windows-gnu', '-x', 'c++', '-std=c++14', '-fms-extensions', '-w',
            '-ferror-limit=0', '-D_WIN32', '-DWIN32', '-D_WIN64', '-DWIN64', '-DCOMPILER_MSVC',
            '-DCOMPILER_MSVC64', '-DPLATFORM_64BITS', '-DNDEBUG',
            *[f'-I{r}/{d}' for r in (sdk, lc) for d in ('public', 'public/tier0', 'public/tier1', 'public/mathlib', 'common')],
            '-isystem', '/usr/x86_64-w64-mingw32/include', '-isystem', rd + '/include']
    tu = ci.Index.create().parse('probe.cpp', args=args, unsaved_files=[('probe.cpp', src)])
    for d in list(tu.diagnostics)[:8]:
        if d.severity >= ci.Diagnostic.Error:
            print('diag:', d, file=sys.stderr)
    fatal = [d for d in tu.diagnostics if d.severity >= ci.Diagnostic.Fatal]
    if fatal:
        sys.exit('\n'.join(map(str, fatal)))
    return tu


def classes(tu):
    out = {}
    for c in tu.cursor.walk_preorder():
        if c.kind in (ci.CursorKind.CLASS_DECL, ci.CursorKind.STRUCT_DECL) and c.is_definition():
            out.setdefault(c.spelling, c)
    return out


def stack_bytes(m):
    """Bytes a thiscall callee pops: parameters rounded to 4 bytes each, plus
    the hidden return pointer MSVC uses for member functions returning a UDT."""
    total = 0
    for a in m.get_arguments():
        t = a.type.get_canonical()
        if t.kind in (ci.TypeKind.POINTER, ci.TypeKind.LVALUEREFERENCE, ci.TypeKind.RVALUEREFERENCE,
                      ci.TypeKind.CONSTANTARRAY, ci.TypeKind.INCOMPLETEARRAY):
            size = 4
        else:
            size = max(t.get_size(), 4)
        total += (size + 3) & ~3
    if m.result_type.get_canonical().kind == ci.TypeKind.RECORD:
        total += 4
    return total


def method_sig(m):
    params = ', '.join(f'{a.type.spelling} {a.spelling}'.strip() for a in m.get_arguments())
    const = ' const' if m.is_const_method() else ''
    return f'{m.result_type.spelling} {m.spelling}({params}){const}'


def vtable(cls, all_classes, cache={}):
    """Returns list of (name, signature, introducing class) for the primary vtable."""
    if cls.spelling in cache:
        return cache[cls.spelling]
    bases = [b for b in cls.get_children() if b.kind == ci.CursorKind.CXX_BASE_SPECIFIER]
    slots = []
    if bases:
        base = all_classes.get(bases[0].type.get_declaration().spelling)
        if base is not None:
            slots = list(vtable(base, all_classes))
    inherited = {(s[0], s[1].split('(', 1)[1]) for s in slots}  # (name, params)
    inherited_names = {s[0] for s in slots}
    own = []
    for m in cls.get_children():
        if m.kind == ci.CursorKind.DESTRUCTOR and m.is_virtual_method():
            if '~' not in {s[0][:1] for s in slots}:
                own.append(('~' + cls.spelling, f'~{cls.spelling}()', 4))
        elif m.kind == ci.CursorKind.CXX_METHOD and m.is_virtual_method():
            sig = method_sig(m)
            if (m.spelling, sig.split('(', 1)[1]) in inherited:
                continue  # override
            own.append((m.spelling, sig, stack_bytes(m)))
    # MSVC: group overloads at the first occurrence, reversed.
    ordered, seen = [], set()
    for name, sig, _ in own:
        if name in seen:
            continue
        seen.add(name)
        group = [o for o in own if o[0] == name]
        if name in inherited_names:
            # New overloads of an inherited name are appended after the base.
            pass
        ordered.extend(reversed(group))
    result = slots + [(n, s, cls.spelling, b) for n, s, b in ordered]
    cache[cls.spelling] = result
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--sdk', required=True)
    ap.add_argument('--header', action='append', default=[])
    ap.add_argument('--skip-appsystem', type=int, default=0,
                    help='drop this many IAppSystem slots (L4D2 has 5 of Swarm\'s 8)')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('classes', nargs='+')
    a = ap.parse_args()
    tu = parse(a.sdk, a.header)
    allc = classes(tu)
    for name in a.classes:
        c = allc.get(name)
        if c is None:
            print(f'{name}: not found')
            continue
        v = vtable(c, allc)
        if a.skip_appsystem:
            drop = {'GetDependencies', 'GetTier', 'Reconnect', 'IsSingleton'}
            v = [e for e in v if not (e[2] == 'IAppSystem' and e[0] in drop)]
        if a.json:
            import json
            print(json.dumps([{'name': n, 'sig': sig, 'owner': o, 'stack': b} for n, sig, o, b in v]))
            continue
        print(f'{name}: {len(v)} slots')
        for i, (n, sig, owner, b) in enumerate(v):
            print(f'  [{i:3d}] {i * 4:#05x} {owner:24s} {b:3d}  {sig}')


if __name__ == '__main__':
    main()
