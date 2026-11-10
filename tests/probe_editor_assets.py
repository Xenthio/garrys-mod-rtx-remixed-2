"""Source asset fixtures: no installed game or GUI required."""

import io
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from probe_editor.assets import Assets, Vpk, asset_name, decode_vtf
from probe_editor import document, storage, math3d, geometry


def vtf(width, height, fmt, pixels, minor=2, mips=1):
    header = bytearray(88 if minor >= 3 else 80)
    header[:4] = b"VTF\0"
    struct.pack_into("<3I", header, 4, 7, minor, len(header))
    struct.pack_into("<2H", header, 16, width, height)
    struct.pack_into("<H", header, 24, 1)
    struct.pack_into("<I", header, 52, fmt)
    header[56] = mips
    struct.pack_into("<I", header, 57, 0xFFFFFFFF)
    struct.pack_into("<H", header, 63, 1)
    if minor >= 3:
        struct.pack_into("<I", header, 68, 1)
        header[80:84] = b"\x30\0\0\0"
        struct.pack_into("<I", header, 84, 96)
        header.extend(b"\0" * 8)
    return bytes(header) + pixels


class Tests(unittest.TestCase):
    def test_bsp_material_coordinates_and_nodraw(self):
        face, model, info = bytearray(56), bytearray(48), bytearray(72)
        struct.pack_into("<ih", face, 4, 0, 3)
        struct.pack_into("<2i", model, 40, 0, 1)
        struct.pack_into("<8f", info, 0, 1, 0, 0, 64, 0, 1, 0, 32)
        lumps = {
            2: struct.pack("<3f5i", 0, 0, 0, 0, 128, 64, 128, 64),
            3: struct.pack("<9f", 0, 0, 0, 128, 0, 0, 0, 128, 0),
            6: info,
            7: face,
            12: struct.pack("<6H", 0, 1, 1, 2, 2, 0),
            13: struct.pack("<3i", 0, 1, 2),
            14: model,
            43: b"fixtures/brick\0",
            44: struct.pack("<i", 0),
        }
        data = bytearray(1036)
        data[:4] = b"VBSP"
        struct.pack_into("<i", data, 4, 20)
        for index, payload in lumps.items():
            struct.pack_into("<4i", data, 8 + index * 16, len(data), len(payload), 0, 0)
            data.extend(payload)
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "test.bsp"
            path.write_bytes(data)
            world = geometry.load(path, textured=True)
            self.assertEqual(world["batches"][0]["material"], "fixtures/brick")
            self.assertEqual(world["texturedMesh"][6:8], [0.5, 0.5])
            self.assertEqual(world["texturedMesh"][14:16], [1.5, 0.5])
            offset = struct.unpack_from("<i", data, 8 + 6 * 16)[0]
            struct.pack_into("<I", data, offset + 64, 128)
            path.write_bytes(data)
            self.assertEqual(geometry.load(path, textured=True)["texturedMesh"], [])

    def test_vtf_raw_and_block_mips(self):
        rgba = bytes([20, 40, 60, 255, 70, 80, 90, 255])
        image = decode_vtf(vtf(2, 1, 0, rgba))
        self.assertEqual(image.getpixel((1, 0)), (70, 80, 90, 255))
        red = struct.pack("<HHI", 0xF800, 0, 0)
        blue = struct.pack("<HHI", 0x001F, 0, 0)
        image = decode_vtf(vtf(4, 4, 13, red))
        self.assertEqual(image.getpixel((2, 2)), (255, 0, 0, 255))
        image = decode_vtf(vtf(8, 8, 13, blue + red * 4, minor=3, mips=2), maximum=4)
        self.assertEqual(image.size, (4, 4))
        self.assertEqual(image.getpixel((2, 2)), (0, 0, 255, 255))
        with self.assertRaises(ValueError):
            decode_vtf(vtf(4, 4, 13, red[:-1]))

    def test_vpk_versions_preload_and_archive(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for version in (1, 2):
                for archive in (0x7FFF, 0):
                    entry = struct.pack("<IHHIIH", 0, 3, archive, 0, 4, 0xFFFF) + b"abc"
                    tree = b"vtf\0materials/test\0image\0" + entry + b"\0\0\0"
                    header = struct.pack("<3I", 0x55AA1234, version, len(tree))
                    if version == 2:
                        header += b"\0" * 16
                    path = root / "pak_dir.vpk"
                    path.write_bytes(
                        header + tree + (b"defg" if archive == 0x7FFF else b"")
                    )
                    (root / "pak_000.vpk").write_bytes(b"defg")
                    self.assertEqual(
                        Vpk(path).read("materials/test/image.vtf"), b"abcdefg"
                    )

    def test_pak_override_and_material_patch(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "materials").mkdir()
            (root / "materials/base.vmt").write_text(
                'LightmappedGeneric { "$basetexture" "brick/base" }'
            )
            data = io.BytesIO()
            with zipfile.ZipFile(data, "w") as pak:
                pak.writestr(
                    "materials/patch.vmt",
                    'patch { include "materials/base.vmt" replace { "$basetexture" "brick/override" } }',
                )
                pak.writestr("materials/base.vtf", b"pak")
            assets = Assets([root], data.getvalue())
            self.assertEqual(assets.material("patch")["$basetexture"], "brick/override")
            self.assertEqual(assets.read("materials/base.vtf"), b"pak")
        for invalid in ("../secret", "materials/../../secret", "c:/secret"):
            with self.assertRaises(ValueError):
                asset_name(invalid)

    def test_shared_storage_rejects_before_writing(self):
        with tempfile.TemporaryDirectory() as folder:
            output, runtime = Path(folder) / "map.json", Path(folder) / "map.bin"
            value = document.empty("map", "f" * 64)
            value["volumes"] = [document.new_volume([0, 0, 0])]
            storage.save(value, "f" * 64, output, runtime)
            before = output.read_bytes(), runtime.read_bytes()
            value["volumes"][0]["capture"] = [1000, 0, 0]
            with self.assertRaises(ValueError):
                storage.save(value, "f" * 64, output, runtime)
            self.assertEqual(before, (output.read_bytes(), runtime.read_bytes()))

    def test_native_transform_and_camera_invariants(self):
        for rotation in ([0, 0, 0], [23, -44, 71], [90, 90, 45]):
            for axis in math3d.axes(rotation):
                turned = math3d.rotate(rotation, axis, 0.7)
                restored = math3d.axes(math3d.rotate(turned, axis, -0.7))
                for a, b in zip(restored, math3d.axes(rotation)):
                    for x, y in zip(a, b):
                        self.assertAlmostEqual(x, y, places=6)
        volume = document.new_volume([10, 20, 30])
        volume["rotationDegrees"] = [23, -44, 71]
        for axis in range(3):
            direction = math3d.axes(volume["rotationDegrees"])[axis]
            for sign in (-1, 1):
                after = math3d.resize_face(volume, axis, sign, 500)
                opposite = math3d.sub(
                    volume["center"],
                    math3d.mul(direction, sign * volume["halfSize"][axis]),
                )
                actual = math3d.sub(
                    after["center"],
                    math3d.mul(direction, sign * after["halfSize"][axis]),
                )
                for a, b in zip(opposite, actual):
                    self.assertAlmostEqual(a, b, places=6)
        camera = math3d.Camera()
        camera.position = [10, 20, 30]
        forward, right, up = camera.basis()
        point = math3d.add(
            camera.position, math3d.add(math3d.mul(forward, 300), math3d.mul(right, 40))
        )
        pixel = camera.project(point, 1200, 800)
        ray = camera.ray(pixel[:2], 1200, 800)
        hit = math3d.plane_point(camera.position, ray, point, forward)
        for a, b in zip(point, hit):
            self.assertAlmostEqual(a, b, places=6)


if __name__ == "__main__":
    unittest.main()
