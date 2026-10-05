#!/usr/bin/env python3
"""List MSVC RTTI vtables of an x64 PE (PE32+): class, subobject offset, slots.

x64 RTTI is image-relative: a Complete Object Locator (signature 1) holds the
RVAs of its type descriptor and of itself, and each vtable is preceded by an
8-byte pointer to its COL. Slots are counted while entries point into code.

Usage: rtti_vtables64.py <dll> [--class REGEX] [--slots]
"""
import argparse
import re
import struct


class PE:
    def __init__(self, path):
        self.data = d = open(path, 'rb').read()
        pe = struct.unpack_from('<I', d, 0x3c)[0]
        nsec = struct.unpack_from('<H', d, pe + 6)[0]
        opt = struct.unpack_from('<H', d, pe + 20)[0]
        assert struct.unpack_from('<H', d, pe + 24)[0] == 0x20b, 'not PE32+'
        self.base = struct.unpack_from('<Q', d, pe + 24 + 24)[0]
        self.sections = []
        for k in range(nsec):
            name, vsz, va, rsz, rp = struct.unpack_from('<8sIIII', d, pe + 24 + opt + 40 * k)
            chars = struct.unpack_from('<I', d, pe + 24 + opt + 40 * k + 36)[0]
            self.sections.append((name.rstrip(b'\0').decode(), va, max(vsz, rsz), rp, rsz, chars))

    def rva_of(self, off):
        for _, va, _, rp, rsz, _ in self.sections:
            if rp <= off < rp + rsz:
                return va + off - rp
        return None

    def is_code_va(self, va):
        rva = va - self.base
        return any(v <= rva < v + size and chars & 0x20000000 for _, v, size, _, _, chars in self.sections)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dll')
    ap.add_argument('--class', dest='cls')
    ap.add_argument('--slots', action='store_true', help='print slot RVAs')
    a = ap.parse_args()
    pe, d = PE(a.dll), None
    d = pe.data
    # Type descriptors: pVFTable(8), spare(8), name ".?AV...@@".
    tds = {}
    for m in re.finditer(rb'\.\?A[VU][^\0]+\0', d):
        rva = pe.rva_of(m.start())
        if rva is not None:
            tds[rva - 16] = m.group(0)[:-1].decode('latin1')
    # COLs: signature 1, offset, cdOffset, pTypeDescriptor, pClassDescriptor, pSelf (all RVAs).
    cols = {}
    for s in pe.sections:
        if s[0] not in ('.rdata', '.data'):
            continue
        for o in range(s[3], s[3] + s[4] - 24, 4):
            sig, off_, cd, ptd, pcd, pself = struct.unpack_from('<IIIIII', d, o)
            if sig == 1 and ptd in tds and pself == s[1] + o - s[3]:
                cols[pe.base + pself] = (tds[ptd], off_)
    rows = []
    for s in pe.sections:
        if s[0] != '.rdata':
            continue
        for o in range(s[3], s[3] + s[4] - 16, 8):
            p = struct.unpack_from('<Q', d, o)[0]
            if p not in cols:
                continue
            vt = o + 8
            slots = []
            while vt + 8 <= s[3] + s[4]:
                e = struct.unpack_from('<Q', d, vt)[0]
                if not pe.is_code_va(e):
                    break
                slots.append(e - pe.base)
                vt += 8
            name, off_ = cols[p]
            rows.append((name, off_, s[1] + o + 8 - s[3], slots))
    for name, off_, vt_rva, slots in sorted(rows):
        if a.cls and not re.search(a.cls, name):
            continue
        print(f'{name:70s} off={off_:#x} vtable_rva={vt_rva:#x} slots={len(slots)}')
        if a.slots:
            for i, r in enumerate(slots):
                print(f'  [{i:3d}] {r:#x}')


if __name__ == '__main__':
    main()
