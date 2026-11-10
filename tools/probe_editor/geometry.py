"""Read bounded world-brush preview geometry using the existing BSP inspector."""

import hashlib
import math
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from probe_bounds import Bsp


def load(path, game=None, textured=False, additional_roots=()):
    bsp = Bsp(path)
    vertices = list(struct.iter_unpack("<3f", bsp.lump(3)))
    edges = list(struct.iter_unpack("<2H", bsp.lump(12)))
    surfedges = [entry[0] for entry in struct.iter_unpack("<i", bsp.lump(13))]
    faces, models = bsp.lump(7), bsp.lump(14)
    texinfo = bsp.lump(6)
    texdata, names, table = bsp.lump(2), bsp.lump(43), bsp.lump(44)
    materials, textured_groups = {}, {}

    def material_info(texture):
        if not 0 <= texture < len(texinfo) // 72:
            return "__missing__", 128, 128, None
        vectors = struct.unpack_from("<8f", texinfo, texture * 72)
        data_index = struct.unpack_from("<i", texinfo, texture * 72 + 68)[0]
        if not 0 <= data_index < len(texdata) // 32:
            return "__missing__", 128, 128, vectors
        name_index, width, height = struct.unpack_from(
            "<3i", texdata, data_index * 32 + 12
        )
        if not 0 <= name_index < len(table) // 4:
            return "__missing__", 128, 128, vectors
        offset = struct.unpack_from("<i", table, name_index * 4)[0]
        end = names.find(b"\0", offset) if 0 <= offset < len(names) else -1
        if end < 0:
            raise ValueError("Invalid BSP material name")
        return (
            names[offset:end].decode("utf-8", errors="replace"),
            max(1, width),
            max(1, height),
            vectors,
        )

    if len(texinfo) % 72:
        raise ValueError("Unsupported BSP texture-info layout")
    if len(faces) % 56 or len(models) < 48:
        raise ValueError("Unsupported BSP world face/model layout")
    first, count = struct.unpack_from("<2i", models, 40)
    if first < 0 or count < 0 or first + count > len(faces) // 56 or count > 1000000:
        raise ValueError("Invalid world face range")
    lines, mesh, used = [], [], set()
    for face in range(first, first + count):
        texture = struct.unpack_from("<h", faces, face * 56 + 10)[0]
        if 0 <= texture < len(texinfo) // 72:
            flags = struct.unpack_from("<I", texinfo, texture * 72 + 64)[0]
            if flags & 0x86:  # SURF_SKY2D | SURF_SKY | SURF_NODRAW
                continue
        start, length = struct.unpack_from("<ih", faces, face * 56 + 4)
        if start < 0 or length < 0 or start + length > len(surfedges):
            raise ValueError("Invalid world face edges")
        polygon = []
        material, tex_width, tex_height, vectors = (
            material_info(texture) if textured else ("", 128, 128, None)
        )
        for signed_edge in surfedges[start : start + length]:
            edge = abs(signed_edge)
            if edge >= len(edges):
                raise ValueError("Invalid world edge")
            a, b = edges[edge]
            if max(a, b) >= len(vertices):
                raise ValueError("Invalid world vertex")
            segment = [*vertices[a], *vertices[b]]
            if not all(math.isfinite(v) and abs(v) <= 1e8 for v in segment):
                raise ValueError("Invalid world coordinates")
            polygon.append(vertices[b if signed_edge < 0 else a])
            if edge not in used:
                used.add(edge)
                lines.append(segment)
        # BSP brush faces are convex; a fan preserves their winding. Rendering is
        # double sided so the editor remains usable both inside and outside rooms.
        for i in range(1, len(polygon) - 1):
            triangle = [polygon[0], polygon[i], polygon[i + 1]]
            u, v = [[p[j] - triangle[0][j] for j in range(3)] for p in triangle[1:]]
            normal = [
                u[1] * v[2] - u[2] * v[1],
                u[2] * v[0] - u[0] * v[2],
                u[0] * v[1] - u[1] * v[0],
            ]
            length = math.hypot(*normal)
            if length < 1e-8:
                continue
            normal = [n / length for n in normal]
            for point in triangle:
                mesh.extend([*point, *normal])
                if textured:
                    uv = (
                        [
                            (
                                sum(point[j] * vectors[row * 4 + j] for j in range(3))
                                + vectors[row * 4 + 3]
                            )
                            / (tex_width if row == 0 else tex_height)
                            for row in range(2)
                        ]
                        if vectors
                        else [0, 0]
                    )
                    if not all(math.isfinite(v) and abs(v) < 1e9 for v in uv):
                        raise ValueError("Invalid BSP texture coordinates")
                    textured_groups.setdefault(material, []).extend(
                        [*point, *normal, *uv]
                    )
    result = dict(
        name=path.stem,
        sha256=hashlib.sha256(bsp.data).hexdigest(),
        lines=lines,
        mesh=mesh,
        note="World brushes and displacement base polygons; props and material proxies are not shown.",
    )
    if textured:
        from .assets import Assets, content_roots, decode_vtf

        assets = Assets([*additional_roots, *content_roots(game)], bsp.lump(40))
        result.update(
            texturedMesh=[], batches=[], images=materials, warnings=assets.warnings
        )
        for name, group in textured_groups.items():
            first = len(result["texturedMesh"]) // 8
            result["texturedMesh"].extend(group)
            result["batches"].append(
                dict(first=first, count=len(group) // 8, material=name)
            )
            try:
                info = assets.material(name)
                texture = info.get("$basetexture")
                data = (
                    assets.read("materials/" + texture.removesuffix(".vtf") + ".vtf")
                    if texture
                    else None
                )
                materials[name] = decode_vtf(data) if data else None
                if materials[name] is None:
                    result["warnings"].append(f"{name}: missing base texture")
            except (OSError, ValueError, struct.error) as error:
                materials[name] = None
                result["warnings"].append(f"{name}: {error}")
    return result
