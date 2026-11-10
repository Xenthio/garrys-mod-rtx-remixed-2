"""Read-only Source preview assets. Missing/unsupported materials stay explicit."""

import io
import re
import struct
import zipfile
from pathlib import Path, PurePosixPath

from PIL import Image

MAX_ASSET = 64 << 20


def asset_name(value):
    value = value.replace("\\", "/").lower().strip("/")
    path = PurePosixPath(value)
    if not value or ":" in value or any(part in (".", "..") for part in path.parts):
        raise ValueError("Invalid material asset path")
    return value


class Vpk:
    """Index directory trees; numbered archives are opened only for requested assets."""

    def __init__(self, path):
        self.path = Path(path)
        with self.path.open("rb") as stream:
            signature, version, size = struct.unpack("<3I", stream.read(12))
            if signature != 0x55AA1234 or version not in (1, 2) or size > MAX_ASSET:
                raise ValueError("Unsupported VPK directory")
            header = 12 if version == 1 else 28
            stream.seek(header)
            tree = stream.read(size)
        if len(tree) != size:
            raise ValueError("Truncated VPK directory")
        self.data_start = header + size
        self.entries = {}
        cursor = 0

        def string():
            nonlocal cursor
            end = tree.find(b"\0", cursor)
            if end < 0:
                raise ValueError("Unterminated VPK name")
            value = tree[cursor:end].decode("utf-8")
            cursor = end + 1
            return value

        while extension := string():
            while folder := string():
                while name := string():
                    if cursor + 18 > len(tree):
                        raise ValueError("Truncated VPK entry")
                    _, preload, archive, offset, length, terminator = (
                        struct.unpack_from("<IHHIIH", tree, cursor)
                    )
                    cursor += 18
                    if terminator != 0xFFFF or cursor + preload > len(tree):
                        raise ValueError("Invalid VPK entry")
                    prefix = "" if folder == " " else folder + "/"
                    suffix = "" if extension == " " else "." + extension
                    key = asset_name(prefix + name + suffix)
                    self.entries[key] = (
                        tree[cursor : cursor + preload],
                        archive,
                        offset,
                        length,
                    )
                    cursor += preload

    def read(self, name):
        entry = self.entries.get(name)
        if entry is None:
            return None
        preload, archive, offset, length = entry
        if length + len(preload) > MAX_ASSET:
            raise ValueError("VPK asset exceeds preview budget")
        path = (
            self.path
            if archive == 0x7FFF
            else self.path.with_name(
                self.path.name.removesuffix("dir.vpk") + f"{archive:03}.vpk"
            )
        )
        with path.open("rb") as stream:
            stream.seek(offset + (self.data_start if archive == 0x7FFF else 0))
            data = stream.read(length)
        if len(data) != length:
            raise ValueError("Truncated VPK asset")
        return preload + data


class Assets:
    def __init__(self, roots=(), pak=b""):
        self.roots = [Path(root).resolve() for root in roots]
        self.archives = []
        self.warnings = []
        self.pak = zipfile.ZipFile(io.BytesIO(pak)) if pak else None
        self.pak_names = (
            {asset_name(name): name for name in self.pak.namelist()} if self.pak else {}
        )
        for root in self.roots:
            for path in sorted(root.glob("*_dir.vpk")):
                # Sound packs contain no editor materials and can have large indices.
                if "sound" in path.name:
                    continue
                try:
                    self.archives.append(Vpk(path))
                except (OSError, ValueError, struct.error) as error:
                    self.warnings.append(f"{path.name}: {error}")

    def read(self, name):
        name = asset_name(name)
        if name in self.pak_names:
            info = self.pak.getinfo(self.pak_names[name])
            if info.file_size > MAX_ASSET:
                raise ValueError("BSP pak asset exceeds preview budget")
            return self.pak.read(info)
        for root in self.roots:
            path = root / name
            if path.is_file() and path.resolve().is_relative_to(root):
                if path.stat().st_size > MAX_ASSET:
                    raise ValueError("Loose asset exceeds preview budget")
                return path.read_bytes()
        for archive in self.archives:
            value = archive.read(name)
            if value is not None:
                return value
        return None

    def material(self, name, depth=0):
        if depth > 8:
            raise ValueError("Recursive VMT patch")
        name = asset_name(name).removesuffix(".vmt")
        data = self.read("materials/" + name + ".vmt")
        if data is None:
            return {}
        text = re.sub(r"//[^\r\n]*", "", data.decode("utf-8-sig", errors="replace"))
        tokens = re.findall(r'"([^"\r\n]*)"|([^\s{}"]+)', text)
        tokens = [(quoted or bare).lower() for quoted, bare in tokens]
        values = {}
        if "include" in tokens:
            index = tokens.index("include")
            if index + 1 < len(tokens):
                include = tokens[index + 1].removeprefix("materials/")
                values.update(self.material(include, depth + 1))
        for key in ("$basetexture", "$translucent", "$alphatest"):
            if key in tokens and tokens.index(key) + 1 < len(tokens):
                values[key] = tokens[tokens.index(key) + 1]
        return values


def decode_vtf(data, maximum=512):
    """Decode one preview mip/frame, honoring 7.3+ image resource offsets."""
    if len(data) < 64 or data[:4] != b"VTF\0":
        raise ValueError("Invalid VTF header")
    major, minor, header_size = struct.unpack_from("<3I", data, 4)
    width, height = struct.unpack_from("<2H", data, 16)
    flags, frames = struct.unpack_from("<IH", data, 20)
    image_format = struct.unpack_from("<I", data, 52)[0]
    mips = data[56]
    if (
        major != 7
        or minor > 5
        or not width
        or not height
        or max(width, height) > 16384
        or not 1 <= mips <= 16
    ):
        raise ValueError("Unsupported VTF dimensions/version")
    depth = (
        struct.unpack_from("<H", data, 63)[0] if minor >= 2 and len(data) >= 65 else 1
    )
    if depth > 1 or flags & 0x4000:
        raise ValueError("Volume/environment VTF is not a surface preview")
    blocks = {13: (8, 1), 20: (8, 1), 14: (16, 2), 15: (16, 3)}
    raw = {
        0: (4, "RGBA"),
        1: (4, "ABGR"),
        2: (3, "RGB"),
        3: (3, "BGR"),
        11: (4, "ARGB"),
        12: (4, "BGRA"),
        16: (4, "BGRX"),
        5: (1, "L"),
    }
    if image_format not in blocks and image_format not in raw:
        raise ValueError(f"Unsupported VTF format {image_format}")

    def size(w, h):
        return (
            ((w + 3) // 4) * ((h + 3) // 4) * blocks[image_format][0]
            if image_format in blocks
            else w * h * raw[image_format][0]
        )

    offset = None
    if minor >= 3:
        if len(data) < 80:
            raise ValueError("Truncated VTF resources")
        count = struct.unpack_from("<I", data, 68)[0]
        if count > 32 or 80 + count * 8 > min(header_size, len(data)):
            raise ValueError("Invalid VTF resources")
        for resource in range(count):
            tag = data[80 + resource * 8 : 84 + resource * 8]
            if tag[:3] == b"\x30\0\0" and not tag[3] & 2:
                offset = struct.unpack_from("<I", data, 84 + resource * 8)[0]
    if offset is None:
        low_format = struct.unpack_from("<I", data, 57)[0]
        low_w, low_h = data[61:63]
        low_size = (
            ((low_w + 3) // 4) * ((low_h + 3) // 4) * 8 if low_format in (13, 20) else 0
        )
        if low_w and low_h and low_format not in (13, 20, 0xFFFFFFFF):
            raise ValueError("Unsupported VTF thumbnail format")
        offset = header_size + low_size
    chosen = 0
    while max(width >> chosen, height >> chosen) > maximum and chosen + 1 < mips:
        chosen += 1
    for level in range(mips - 1, chosen, -1):
        offset += size(max(1, width >> level), max(1, height >> level)) * max(frames, 1)
    w, h = max(1, width >> chosen), max(1, height >> chosen)
    amount = size(w, h)
    if offset < header_size or offset + amount > len(data) or w * h * 4 > MAX_ASSET:
        raise ValueError("Truncated VTF image")
    pixels = data[offset : offset + amount]
    if image_format in blocks:
        image = Image.frombytes("RGBA", (w, h), pixels, "bcn", blocks[image_format][1])
    else:
        channels, order = raw[image_format]
        mode = "L" if order == "L" else "RGB" if channels == 3 else "RGBA"
        image = Image.frombytes(mode, (w, h), pixels, "raw", order).convert("RGBA")
    image.thumbnail((maximum, maximum))
    return image


def content_roots(game):
    if game is None:
        return []
    game = Path(game)
    return [game / "garrysmod", game / "sourceengine", game / "platform"]
