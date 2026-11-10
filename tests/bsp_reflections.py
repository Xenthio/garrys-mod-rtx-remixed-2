"""Authored projection references preserve source semantics and reject partial data."""
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
from probe_editor.bsp_reflections import entities, reference
from probe_editor.bsp_receivers import material_receivers
from probe_editor.bsp_captures import capture_assignments

HASH = 'a' * 64
CUBE = b'{ "classname" "env_cubemap" "parallax" "1" "origin" "10 20 30" "boxmin" "-2 -3 -4" "boxmax" "5 6 7" "hammerid" "77" "sides" "128912 229012" }'


class CompiledReceivers(unittest.TestCase):
    def fixture(self):
        lumps = {7: bytearray(56 * 3), 6: bytearray(72), 2: bytearray(32),
                 43: b'maps/test/patched_material\0', 44: bytearray(4)}
        struct.pack_into('<h', lumps[7], 56 + 10, -1)
        return lumps

    def read(self, lumps):
        class Bsp:
            def lump(self, index):
                return lumps[index]
        return material_receivers(Bsp())

    def test_actual_compiled_indices_grouped_and_untextured_face_skipped(self):
        self.assertEqual(self.read(self.fixture()), [dict(
            material='maps/test/patched_material', compiledFaceIndices=[0, 2])])

    def test_invalid_assignment_chain_rejects_instead_of_guessing(self):
        for lump, offset, encoding, value in ((7, 10, '<h', 1), (7, 10, '<h', -2),
                (6, 68, '<i', 1), (2, 12, '<i', 1), (44, 0, '<i', -1),
                (44, 0, '<i', 999)):
            with self.subTest(lump=lump, value=value):
                lumps = self.fixture()
                struct.pack_into(encoding, lumps[lump], offset, value)
                with self.assertRaises(ValueError):
                    self.read(lumps)
        for lump in (7, 6, 2, 44):
            lumps = self.fixture()
            lumps[lump] += b'x'
            with self.subTest(layout=lump), self.assertRaises(ValueError):
                self.read(lumps)
        lumps = self.fixture()
        lumps[43] = b'unterminated'
        with self.assertRaises(ValueError):
            self.read(lumps)


class CompiledCaptureAssignments(unittest.TestCase):
    patch = b'"patch" { "include" "materials/base.vmt" "replace" { "$envmap" "maps/test/c10_20_30" } }'
    receivers = [dict(material='maps/test/material_999_888_777', compiledFaceIndices=[0, 2])]
    volumes = [dict(capture=[10, 20, 30], entityIndex=7)]

    def read(self, entries, volumes=None):
        stream = io.BytesIO()
        with zipfile.ZipFile(stream, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            for name, contents in entries:
                archive.writestr(name, contents)
        class Bsp:
            def lump(self, index):
                assert index == 40
                return stream.getvalue()
        return capture_assignments(Bsp(), self.receivers, 'test',
                                   self.volumes if volumes is None else volumes)[0]

    def entries(self, payload=None):
        return [('materials/maps/test/material_999_888_777.vmt', self.patch if payload is None else payload)]

    def test_explicit_texture_wins_over_misleading_material_suffix(self):
        result = self.read(self.entries())
        self.assertEqual(result['capture'], [10, 20, 30])
        self.assertEqual(result['projectionEntityIndices'], [7])
        self.assertEqual(result['status'], 'authored-projection')
        self.assertNotIn('influenceMinimum', result)

    def test_other_scalar_replacements_preserve_capture_assignment(self):
        payload = self.patch.replace(b'"$envmap"', b'"$crackmaterial" "maps/test/glass_b" "$envmap"')
        self.assertEqual(self.read(self.entries(payload))['projectionEntityIndices'], [7])

    def test_missing_unsupported_and_nonlocal_are_reported(self):
        self.assertEqual(self.read([])['status'], 'no-packed-material')
        for payload in (self.patch.replace(b'"replace"', b'"insert"'),
                        self.patch.replace(b'"$envmap"', b'"$envmap" "other" "$envmap"'),
                        self.patch + b' trailing', b'"VertexLitGeneric" { "$envmap" "env_cubemap" }'):
            result = self.read(self.entries(payload))
            self.assertEqual(result['status'], 'unsupported-material-form')
            self.assertNotIn('capture', result)
        result = self.read(self.entries(self.patch.replace(b'maps/test/', b'maps/other/')))
        self.assertEqual(result['status'], 'nonlocal-capture-texture')

    def test_unmatched_and_duplicate_origins_do_not_choose_projection(self):
        self.assertEqual(self.read(self.entries(), [])['status'], 'capture-without-authored-projection')
        result = self.read(self.entries(), self.volumes + [dict(capture=[10, 20, 30], entityIndex=9)])
        self.assertEqual(result['status'], 'ambiguous-projection')
        self.assertEqual(result['projectionEntityIndices'], [7, 9])

    def test_duplicate_paths_traversal_and_read_budgets_reject(self):
        entries = self.entries()
        for extra in [('MATERIALS/maps/test/material_999_888_777.VMT', self.patch),
                      ('materials/../escape.vmt', self.patch)]:
            with self.assertRaises(ValueError):
                self.read(entries + [extra])
        for name in ('MAX_PATCH_BYTES', 'MAX_TOTAL_BYTES', 'MAX_ARCHIVE_ENTRIES'):
            with patch('probe_editor.bsp_captures.' + name, 0), self.assertRaises(ValueError):
                self.read(entries)


class BspReflectionReference(unittest.TestCase):
    def test_preserves_projection_and_hammer_assignments(self):
        report = reference(CUBE, 'test', HASH)
        self.assertEqual(report['mapSha256'], HASH)
        self.assertEqual(report['cubemapEntityCount'], 1)
        record = report['volumes'][0]
        self.assertEqual(record['capture'], [10, 20, 30])
        self.assertEqual(record['projectionMinimumOffset'], [-2, -3, -4])
        self.assertEqual(record['projectionMaximumOffset'], [5, 6, 7])
        self.assertEqual(record['assignedHammerSides'], ['128912', '229012'])
        self.assertEqual(record['hammerId'], '77')
        self.assertTrue(record['captureInside'])
        self.assertNotIn('influenceMinimum', record)
        self.assertNotIn('priority', record)

    def test_only_explicit_parallax_qualifies(self):
        ordinary = CUBE.replace(b'"parallax" "1"', b'"parallax" "0"')
        absent = CUBE.replace(b'"parallax" "1"', b'')
        report = reference(ordinary + absent + CUBE, 'test', HASH)
        self.assertEqual(report['cubemapEntityCount'], 3)
        self.assertEqual(len(report['volumes']), 1)
        self.assertEqual(report['volumes'][0]['entityIndex'], 2)

    def test_angles_and_outside_capture_remain_reference_facts(self):
        payload = CUBE.replace(b'"boxmin" "-2 -3 -4"', b'"boxmin" "1 2 3" "angles" "0 90 0"')
        record = reference(payload, 'test', HASH)['volumes'][0]
        self.assertEqual(record['sourceAngles'], [0, 90, 0])
        self.assertFalse(record['captureInside'])

    def test_quoted_braces_comments_and_duplicate_outputs(self):
        payload = b'// ignored {\n{ "classname" "logic_relay" "OnTrigger" "a{b}" "OnTrigger" "c" }' + CUBE + b'\0'
        self.assertEqual(len(list(entities(payload))), 2)
        self.assertEqual(reference(payload, 'test', HASH)['volumes'][0]['entityIndex'], 1)

    def test_malformed_or_ambiguous_data_rejects(self):
        for payload in [CUBE[:-1], CUBE + b'garbage', CUBE + b'{', b'{ { } }',
                        CUBE.replace(b'"origin" "10 20 30"', b'"origin" "nan 20 30"'),
                        CUBE.replace(b'"boxmax" "5 6 7"', b'"boxmax" "-5 -6 -7"'),
                        CUBE.replace(b'"sides" "128912 229012"', b'"sides" "x"'),
                        CUBE.replace(b'"hammerid" "77"', b'"origin" "1 2 3"'),
                        CUBE.replace(b'"parallax" "1"', b'"parallax" "1" "parallax" "0"'),
                        CUBE.replace(b'"classname" "env_cubemap"', b'"classname" "env_cubemap" "classname" "other"')]:
            with self.subTest(payload=payload), self.assertRaises(ValueError):
                reference(payload, 'test', HASH)
        with self.assertRaises(ValueError):
            reference(CUBE, 'test', 'wrong fingerprint')
        with self.assertRaises(ValueError):
            reference(b' ' * ((16 << 20) + 1), 'test', HASH)

    def test_reference_budget_declines_instead_of_truncating(self):
        with patch('probe_editor.bsp_reflections.MAX_VOLUMES', 1):
            with self.assertRaises(ValueError):
                reference(CUBE + CUBE, 'test', HASH)

    def test_cli_uses_actual_bsp_fingerprint(self):
        data = bytearray(1036)
        data[:4] = b'VBSP'
        struct.pack_into('<i', data, 4, 20)
        struct.pack_into('<4i', data, 8, len(data), len(CUBE), 0, 0)
        data.extend(CUBE)
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'test.bsp'
            output = Path(folder) / 'reference.json'
            path.write_bytes(data)
            env = dict(os.environ, PYTHONPATH=str(TOOLS))
            subprocess.run([sys.executable, '-m', 'probe_editor.bsp_reflections', str(path),
                            '--capture-assignments', '--output', str(output)],
                           env=env, check=True, capture_output=True)
            report = json.loads(output.read_text())
            self.assertEqual(report['mapSha256'], hashlib.sha256(data).hexdigest())
            self.assertEqual(len(report['volumes']), 1)
            self.assertEqual(report['compiledMaterialReceivers'], [])
            self.assertEqual(report['compiledCaptureAssignments'], [])


if __name__ == '__main__':
    unittest.main()
