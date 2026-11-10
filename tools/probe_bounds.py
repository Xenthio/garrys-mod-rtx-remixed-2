"""Plot published reflection boxes against a planar section of Source BSP brushes.

Requires matplotlib. Reads world-model faces only: props, moving brush models
and displaced terrain are deliberately omitted. This is a geometry inspection
aid, not a visibility or reflection-quality test.
"""
import argparse
import colorsys
import hashlib
import json
import lzma
import math
from pathlib import Path
import struct


class Bsp:
    def __init__(self, path):
        self.data = path.read_bytes()
        if len(self.data) < 1036 or self.data[:4] != b"VBSP":
            raise ValueError("Not a Source BSP")
        version = struct.unpack_from("<i", self.data, 4)[0]
        if version not in (19, 20, 21):
            raise ValueError(f"Unsupported BSP version {version}")
        first = struct.unpack_from("<i", self.data, 8)[0]
        self.version_first = version == 21 and 0 <= first <= 8

    def lump(self, index):
        entry = struct.unpack_from("<4i", self.data, 8 + index * 16)
        offset, size = entry[1:3] if self.version_first else entry[:2]
        if offset < 0 or size < 0 or offset + size > len(self.data):
            raise ValueError(f"Invalid lump {index}")
        data = self.data[offset:offset + size]
        if data[:4] == b"LZMA":
            if len(data) < 17:
                raise ValueError("Truncated Source LZMA header")
            actual, packed = struct.unpack_from("<2I", data, 4)
            dictionary = struct.unpack_from("<I", data, 13)[0]
            if actual > 256 * 1024 * 1024 or dictionary > 128 * 1024 * 1024 or packed > len(data) - 17:
                raise ValueError("Oversized or truncated Source LZMA lump")
            properties = data[12]
            lc, rest = properties % 9, properties // 9
            decoder = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=[{
                "id": lzma.FILTER_LZMA1, "dict_size": dictionary,
                "lc": lc, "lp": rest % 5, "pb": rest // 5,
            }])
            data = decoder.decompress(data[17:17 + packed], max_length=actual + 1)
            if len(data) != actual:
                raise ValueError("Decoded Source LZMA size mismatch")
        return data

    def section(self, axis, level):
        vertices = list(struct.iter_unpack("<3f", self.lump(3)))
        edges = list(struct.iter_unpack("<2H", self.lump(12)))
        surfedges = [value[0] for value in struct.iter_unpack("<i", self.lump(13))]
        faces, models = self.lump(7), self.lump(14)
        if len(faces) % 56 or len(models) < 48:
            raise ValueError("Unsupported face/model layout")
        first, count = struct.unpack_from("<2i", models, 40)
        if first < 0 or count < 0 or first + count > len(faces) // 56:
            raise ValueError("Invalid world-model face range")
        segments = []
        planar = 0
        for index in range(first, first + count):
            at = index * 56
            start, length = struct.unpack_from("<ih", faces, at + 4)
            displacement = struct.unpack_from("<h", faces, at + 12)[0]
            if displacement >= 0 or length < 3:
                continue
            if start < 0 or start + length > len(surfedges):
                raise ValueError("Invalid face edge range")
            polygon = []
            for edge in surfedges[start:start + length]:
                if abs(edge) >= len(edges):
                    raise ValueError("Invalid edge reference")
                vertex = edges[abs(edge)][int(edge < 0)]
                if vertex >= len(vertices):
                    raise ValueError("Invalid vertex reference")
                point = vertices[vertex]
                if not all(math.isfinite(v) for v in point):
                    raise ValueError("Nonfinite world vertex")
                polygon.append(point)
            planar += 1
            segments.extend(section_polygon(polygon, axis, level))
        return segments, planar


def section_polygon(polygon, axis, level):
    axes = [i for i in range(3) if i != axis]
    project = lambda p: tuple(p[i] for i in axes)
    distances = [p[axis] - level for p in polygon]
    if all(abs(d) < 1e-5 for d in distances):
        return [(project(polygon[i - 1]), project(p)) for i, p in enumerate(polygon)]
    points = []
    for i, current in enumerate(polygon):
        previous = polygon[i - 1]
        a, b = distances[i - 1], distances[i]
        if abs(b) < 1e-5:
            points.append(project(current))
        if a * b < 0:
            t = a / (a - b)
            points.append(tuple(previous[c] + t * (current[c] - previous[c]) for c in axes))
    if len(points) < 2:
        return []
    # BSP brush faces are convex. Collinear vertices can repeat the same hit;
    # the farthest pair retains one intersection segment without duplicate edges.
    pair = max(((a, b) for a in points for b in points),
               key=lambda p: sum((a - b) ** 2 for a, b in zip(*p)))
    return [pair] if pair[0] != pair[1] else []


def probe_section(probe, axis, level, influence=False):
    """Intersect a rigid probe-local box with a world-space slice plane."""
    from probe_editor.document import projection_transform

    lo = probe.get('influenceMinimum', probe['minimum']) if influence else probe['minimum']
    hi = probe.get('influenceMaximum', probe['maximum']) if influence else probe['maximum']
    if any(len(v) != 3 or not all(math.isfinite(c) for c in v) for v in (lo, hi)):
        raise ValueError('Invalid probe bounds')
    if any(a >= b for a, b in zip(lo, hi)):
        raise ValueError('Invalid probe bounds')
    projection_transform(probe, lo, hi)  # Validate the same rigid frame accepted by the editor.
    rows = probe.get('worldToLocal', [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0]])
    corners = []
    for corner in range(8):
        local = [(hi[c] if corner & (1 << c) else lo[c]) - rows[c][3] for c in range(3)]
        corners.append(tuple(sum(rows[c][w] * local[c] for c in range(3)) for w in range(3)))
    axes = [c for c in range(3) if c != axis]
    points = []
    for start in range(8):
        for direction in range(3):
            end = start ^ (1 << direction)
            if end < start:
                continue
            a, b = corners[start], corners[end]
            da, db = a[axis] - level, b[axis] - level
            for point, distance in ((a, da), (b, db)):
                if abs(distance) <= 1e-5:
                    points.append(tuple(point[c] for c in axes))
            if da * db < 0:
                t = da / (da - db)
                points.append(tuple(a[c] + t * (b[c] - a[c]) for c in axes))
    unique = []
    for point in points:
        if not any(math.dist(point, other) <= 1e-5 for other in unique):
            unique.append(point)
    if len(unique) < 3:
        return []
    center = [sum(p[c] for p in unique) / len(unique) for c in range(2)]
    return sorted(unique, key=lambda p: math.atan2(p[1] - center[1], p[0] - center[0]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bsp", type=Path)
    parser.add_argument("probes", type=Path, help="Fresh runtime probes.json export")
    parser.add_argument("output", type=Path, help="Fresh .png or .svg path")
    parser.add_argument("--axis", choices="xyz", default="z")
    parser.add_argument("--slice", type=float, required=True, dest="level")
    parser.add_argument("--probe", type=int, action="append", help="Selected index; repeat for multiple boxes")
    parser.add_argument("--receiver", type=float, nargs=3)
    parser.add_argument("--window", type=float, nargs=4, metavar=('LEFT', 'RIGHT', 'BOTTOM', 'TOP'),
                        help="Zoom limits in the two plotted world axes")
    args = parser.parse_args()
    if args.output.exists() or args.output.with_suffix(".json").exists():
        parser.error("Use a fresh output path")
    if args.output.suffix.lower() not in (".png", ".svg") or not math.isfinite(args.level):
        parser.error("Use a finite slice coordinate and .png/.svg output")
    if args.receiver and not all(math.isfinite(v) for v in args.receiver):
        parser.error("Receiver coordinates must be finite")
    if args.window and (not all(math.isfinite(v) for v in args.window) or
                        args.window[0] >= args.window[1] or args.window[2] >= args.window[3]):
        parser.error("Window limits must be finite and ordered")
    source = args.probes.read_bytes()
    report = json.loads(source)
    if report.get("version") != 1 or report.get("activeHash") in (None, 0, "0"):
        parser.error("A published version-1 probe report is required")
    if not math.isfinite(report["metersPerUnit"]) or report["metersPerUnit"] <= 0:
        parser.error("Invalid scene scale")
    selected = args.probe if args.probe is not None else range(len(report["probes"]))
    axis = "xyz".index(args.axis)
    axes = [i for i in range(3) if i != axis]
    probes = []
    for index in selected:
        if index < 0 or index >= len(report["probes"]):
            parser.error(f"Probe {index} is outside the published report")
        probe = report["probes"][index]
        for key in ("position", "minimum", "maximum"):
            if len(probe[key]) != 3 or not all(math.isfinite(v) for v in probe[key]):
                parser.error("Invalid probe coordinates")
        if any(probe["minimum"][i] >= probe["maximum"][i] for i in range(3)):
            parser.error("Invalid projection box")
        try:
            projection = probe_section(probe, axis, args.level)
            influence = probe_section(probe, axis, args.level, influence=True)
        except ValueError as error:
            parser.error(str(error))
        if projection or influence:
            probes.append((index, probe, projection, influence))
    if not probes:
        parser.error("No selected probe intersects this slice")
    bsp = Bsp(args.bsp)
    segments, faces = bsp.section(axis, args.level)

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.collections import LineCollection
    from matplotlib.patches import Polygon

    limits = []
    for component, dimension in enumerate(axes):
        coordinates = [point[component] for _, _, projection, influence in probes
                       for point in projection + influence]
        low, high = min(coordinates), max(coordinates)
        if args.receiver:
            low, high = min(low, args.receiver[dimension]), max(high, args.receiver[dimension])
        pad = max((high - low) * .08, 16)
        limits.append((low - pad, high + pad))
    if args.window:
        limits = [tuple(args.window[:2]), tuple(args.window[2:])]
    aspect = (limits[1][1] - limits[1][0]) / (limits[0][1] - limits[0][0])
    fig, ax = plt.subplots(figsize=(12, min(9, max(3.5, 12 * aspect + 2))))
    ax.add_collection(LineCollection(segments, colors="#30343b", linewidths=.9, zorder=2))
    for index, probe, projection, influence in probes:
        lo, hi = probe["minimum"], probe["maximum"]
        color = colorsys.hsv_to_rgb((index * .61803398875) % 1, .8, .65)
        dimensions = [(hi[i] - lo[i]) * report["metersPerUnit"] for i in range(3)]
        label = f"#{index}: " + " x ".join(f"{d:.1f}" for d in dimensions) + " m"
        if projection:
            ax.add_patch(Polygon(projection, facecolor=(*color, .07), edgecolor=color,
                                 linewidth=2, zorder=3, label=label))
        if influence and influence != projection:
            ax.add_patch(Polygon(influence, fill=False, edgecolor=color, linestyle='--',
                                 linewidth=1.5, zorder=3, label=f"#{index} influence"))
        position = [probe["position"][i] for i in axes]
        ax.plot(*position, marker="x", color=color, zorder=4)
        ax.annotate(f"#{index}", position, xytext=(5, 5), textcoords="offset points", color=color)
    if args.receiver:
        ax.plot(*(args.receiver[i] for i in axes), "*", color="#d62728", markersize=12,
                label="Receiver (projected)", zorder=5)
    ax.set_xlim(*limits[0])
    ax.set_ylim(*limits[1])
    ax.set_aspect("equal")
    ax.set_xlabel("xyz"[axes[0]].upper() + " (Source units)")
    ax.set_ylabel("xyz"[axes[1]].upper() + " (Source units)")
    ax.set_title(f"{args.bsp.stem}: brush section {args.axis.upper()} = {args.level:g}\n"
                 "Lines = world brushes; solid = projection; dashed = independent influence")
    handles, labels = ax.get_legend_handles_labels()
    fig.legend(handles, labels, loc="lower center", bbox_to_anchor=(.5, .06), ncol=3, fontsize=8)
    fig.text(.02, .02, "Capture origins and receiver are projected onto the slice. Props, moving brushes and displacements are omitted.", fontsize=8)
    legend_space = .24 if len(probes) >= 10 else .15
    fig.tight_layout(rect=(0, legend_space, 1, 1))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.output, dpi=160)
    plt.close(fig)
    args.output.with_suffix(".json").write_text(json.dumps({
        "bsp": str(args.bsp.resolve()), "bsp_sha256": hashlib.sha256(bsp.data).hexdigest(),
        "probes": str(args.probes.resolve()), "probes_sha256": hashlib.sha256(source).hexdigest(),
        "activeHash": report["activeHash"], "axis": args.axis, "slice": args.level,
        "selected_probes": [i for i, _, _, _ in probes], "planar_world_faces": faces,
        "intersection_segments": len(segments), "receiver": args.receiver,
        "limits": "Planar world brush geometry only; containment is not reflection-quality proof.",
    }, indent=2) + "\n")
    print(args.output)


if __name__ == "__main__":
    main()
