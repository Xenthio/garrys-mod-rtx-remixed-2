"""Authoring data must not silently cross map revisions or lose transforms."""
import copy
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from probe_editor import document, geometry


class AuthoringData(unittest.TestCase):
    def setUp(self):
        self.value = document.empty('test', 'abc')
        volume = document.new_volume([10, 20, 30])
        volume.update(center=[100, -200, 300], capture=[12, 34, 56],
                      rotationDegrees=[15, 89, -120], priority=50)
        self.value['volumes'].append(volume)

    def test_roundtrip_independent_capture_and_rotation(self):
        result = document.validate(json.loads(json.dumps(self.value)), 'abc')
        self.assertEqual(result, self.value)
        self.assertNotEqual(result['volumes'][0]['capture'], result['volumes'][0]['center'])

    def test_rejects_other_map_and_duplicate_identity(self):
        with self.assertRaises(ValueError):
            document.validate(self.value, 'different map')
        self.value['volumes'].append(copy.deepcopy(self.value['volumes'][0]))
        with self.assertRaises(ValueError):
            document.validate(self.value, 'abc')

    def test_invalid_transforms_and_priorities(self):
        for key, bad in [('center', [0, float('nan'), 0]), ('capture', [True, 0, 0]),
                         ('halfSize', [0, 1, 1]), ('rotationDegrees', [1, 2]),
                         ('priority', 1.5), ('priority', True), ('priority', 32768)]:
            value = copy.deepcopy(self.value)
            value['volumes'][0][key] = bad
            with self.subTest(key=key, bad=bad), self.assertRaises(ValueError):
                document.validate(value, 'abc')

    def test_native_report_import_preserves_host_units_and_provenance(self):
        payload = json.dumps(dict(version=1, activeHash='1234', metersPerUnit=.0254,
            probes=[dict(position=[256, 512, 128], minimum=[0, 128, 0], maximum=[512, 1024, 256])])).encode()
        first = document.from_probe_report(payload, 'test', 'abc')
        second = document.from_probe_report(payload, 'test', 'abc')
        self.assertEqual(first, second)
        volume = first['volumes'][0]
        self.assertEqual(volume['capture'], [256, 512, 128])
        self.assertEqual(volume['center'], [256, 576, 128])
        self.assertEqual(volume['halfSize'], [256, 448, 128])
        volume['center'][0] += 10
        self.assertEqual(document.validate(first, 'abc')['reference'], first['reference'])
        self.assertEqual(volume['capture'][0], 256)

    def test_native_oriented_report_preserves_projection_and_priority(self):
        frame = [[0, 1, 0, -20], [-1, 0, 0, 10], [0, 0, 1, -30]]
        probe = dict(position=[10, 20.25, 30], minimum=[-2, -.5, -1], maximum=[2, .5, 1],
                     worldToLocal=frame, priority=-12)
        def payload():
            return json.dumps(dict(version=1, activeHash='42', metersPerUnit=.0254, probes=[probe])).encode()
        volume = document.from_probe_report(payload(), 'test', 'abc')['volumes'][0]
        self.assertEqual(volume['center'], [10, 20, 30])
        self.assertEqual(volume['halfSize'], [2, .5, 1])
        self.assertEqual(volume['capture'], [10, 20.25, 30])
        self.assertEqual(volume['rotationDegrees'], [0, 0, 90])
        self.assertEqual(volume['priority'], -12)
        frame[0][1] = 2
        with self.assertRaises(ValueError):
            document.from_probe_report(payload(), 'test', 'abc')

    def test_native_report_refuses_unpublished_and_invalid_bounds(self):
        for active, lo, hi in [('0', [0, 0, 0], [1, 1, 1]),
                              ('10', [0, 0, 0], [-1, 1, 1])]:
            payload = json.dumps(dict(version=1, activeHash=active, metersPerUnit=.0254,
                probes=[dict(position=[0, 0, 0], minimum=lo, maximum=hi)])).encode()
            with self.assertRaises(ValueError):
                document.from_probe_report(payload, 'test', 'abc')

    def test_runtime_sidecar_layout_rotation_and_capacity(self):
        import hashlib
        value = document.empty('test', hashlib.sha256(b'abc').hexdigest())
        volume = document.new_volume([10, 20.25, 30])
        volume.update(center=[10, 20, 30], halfSize=[2, .5, 1], rotationDegrees=[0, 0, 90], priority=-12)
        value['volumes'] = [volume]
        encoded = document.runtime_binary(value)
        self.assertEqual(encoded, document.runtime_binary(value))
        self.assertEqual(encoded[:8], b'BSPCUBE1')
        self.assertEqual(encoded[8:40], hashlib.sha256(b'abc').digest())
        self.assertEqual(len(encoded), 44+96)
        self.assertEqual(struct.unpack_from('<I', encoded, 40)[0], 1)
        record = struct.unpack_from('<Q6f12f3fi', encoded, 44)
        self.assertNotEqual(record[0], 0)
        self.assertEqual(record[1:7], (-2, -.5, -1, 2, .5, 1))
        for actual, expected in zip(record[7:19], (0,1,0,-20,-1,0,0,10,0,0,1,-30)):
            self.assertAlmostEqual(actual, expected, places=5)
        self.assertEqual(record[19:22], (10,20.25,30))
        self.assertEqual(record[22], -12)
        volume['capture'] = [100, 20, 30]
        with self.assertRaises(ValueError):
            document.runtime_binary(value)
        value['volumes'] = [dict(document.new_volume([0,0,0]), id=str(i)) for i in range(64)]
        encoded = document.runtime_binary(value)
        self.assertEqual(struct.unpack_from('<I', encoded, 40)[0], 64)
        self.assertEqual(len(encoded), 44 + 64 * 96)
        value['volumes'].append(dict(document.new_volume([0,0,0]), id='overflow'))
        with self.assertRaises(ValueError):
            document.runtime_binary(value)

    def test_probe_report_preserves_independent_influence_and_feather(self):
        probe = dict(position=[1, 2, 3], minimum=[0, 0, 0], maximum=[2, 4, 6],
                     influenceMinimum=[-2, -4, -6], influenceMaximum=[4, 8, 12],
                     featherDistance=[.6, 1.2, 1.8])
        def convert():
            payload = json.dumps(dict(version=1, activeHash='42', metersPerUnit=.0254, probes=[probe])).encode()
            return document.from_probe_report(payload, 'test', 'a'*64)
        result = convert()
        sampling = result['volumes'][0]['sampling']
        self.assertEqual(sampling['minimum'], [-3, -6, -9])
        self.assertEqual(sampling['maximum'], [3, 6, 9])
        self.assertEqual(sampling['blendDistance'], 0)
        self.assertEqual(document.runtime_binary(result)[:8], b'BSPCUBE2')
        probe['featherDistance'] = [2, 2, 2]
        self.assertEqual(convert()['volumes'][0]['sampling']['blendDistance'], 2)
        probe['featherDistance'] = [1, 2, 3]
        with self.assertRaises(ValueError):
            convert()
        probe.update(minimum=[-100]*3, maximum=[100]*3, influenceMinimum=[-100]*3,
                     influenceMaximum=[100]*3, featherDistance=[.5/.0254]*3)
        legacy = convert()
        self.assertNotIn('sampling', legacy['volumes'][0])
        self.assertEqual(document.runtime_binary(legacy)[:8], b'BSPCUBE1')

    def test_independent_sampling_roundtrip_and_binary_compatibility(self):
        value = document.empty('test', 'a' * 64)
        volume = document.new_volume([0, 0, 0])
        value['volumes'] = [volume]
        legacy = document.runtime_binary(value)
        self.assertEqual(legacy[:8], b'BSPCUBE1')
        volume['sampling'] = dict(minimum=[-200, -100, -50], maximum=[300, 100, 75], blendDistance=12)
        clean = document.validate(value, 'a' * 64)
        self.assertEqual(clean, value)
        encoded = document.runtime_binary(clean)
        self.assertEqual(encoded[:8], b'BSPCUBE2')
        self.assertEqual(len(encoded), 44 + 124)
        self.assertEqual(encoded[44:140], legacy[44:140])
        self.assertEqual(struct.unpack_from('<7f', encoded, 140), (-200, -100, -50, 300, 100, 75, 12))
        value['volumes'].append(document.new_volume([0, 0, 0]))
        mixed = document.runtime_binary(value)
        fallback = struct.unpack_from('<7f', mixed, 44 + 124 + 96)
        self.assertEqual(fallback[:6], (-128, -128, -128, 128, 128, 128))
        self.assertAlmostEqual(fallback[6], .5 / .0254, places=5)
        for distance in [-1, float('nan'), True]:
            value['volumes'][0]['sampling']['blendDistance'] = distance
            with self.assertRaises(ValueError):
                document.validate(value, 'a' * 64)

    def test_geometry_from_world_model_only(self):
        # One triangle, plus an unrelated vertex which must not become an edge.
        lumps = {3: struct.pack('<12f', 0, 0, 0, 128, 0, 0, 0, 128, 0, 999, 999, 999),
                 12: struct.pack('<6H', 0, 1, 1, 2, 2, 0),
                 13: struct.pack('<3i', 0, 1, 2)}
        face = bytearray(56)
        struct.pack_into('<ih', face, 4, 0, 3)
        struct.pack_into('<h', face, 12, -1)
        lumps[7] = face
        model = bytearray(48)
        struct.pack_into('<2i', model, 40, 0, 1)
        lumps[14] = model
        contents = bytearray(1036)
        contents[:4] = b'VBSP'
        struct.pack_into('<i', contents, 4, 20)
        for index, payload in lumps.items():
            struct.pack_into('<4i', contents, 8 + index * 16, len(contents), len(payload), 0, 0)
            contents.extend(payload)
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'test.bsp'
            path.write_bytes(contents)
            result = geometry.load(path)
            self.assertEqual(len(result['lines']), 3)
            self.assertEqual(len(result['mesh']), 18)
            self.assertEqual(result['mesh'][0:6], [0, 0, 0, 0, 0, 1])
            self.assertEqual(result['mesh'][6:9], [128, 0, 0])
            self.assertEqual(result['mesh'][12:15], [0, 128, 0])
            self.assertTrue(all(max(edge) <= 128 for edge in result['lines']))
            # A valid lump with a bad edge reference must fail before displaying it.
            struct.pack_into('<i', contents, struct.unpack_from('<i', contents, 8+13*16)[0], 100)
            path.write_bytes(contents)
            with self.assertRaises(ValueError):
                geometry.load(path)


if __name__ == '__main__':
    unittest.main()
