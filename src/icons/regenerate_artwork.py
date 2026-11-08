#!/usr/bin/env python3
"""Regenerate vendored menu icons, country flags and client icons; not needed for normal builds.

Requires picosvg==0.23.0 and rsvg-convert (librsvg). Source SVGs and licenses
live in vendor/. See ARTWORK.md for provenance and the pinned upstream revisions.
"""

from pathlib import Path
from copy import deepcopy
import re
import subprocess
import struct
import zlib

from picosvg.svg import SVG
from picosvg.svg_types import SVGPath
from lxml import etree

ROOT = Path(__file__).resolve().parent


def expand_star_markers(svg):
    """Expand the simple marker-mid star layout in the pinned US/UM originals.

    NanoSVG and picosvg do not implement markers. Reject any other marker form
    so an upstream change cannot silently lose or incorrectly place the stars.
    """
    ns = {"s": "http://www.w3.org/2000/svg"}
    root = svg.svg_root
    for marker in list(root.findall("s:marker", ns)):
        assert set(marker.attrib) == {"id", "markerHeight", "markerWidth"}
        assert marker.get("markerHeight") == marker.get("markerWidth") == "30"
        uses = root.xpath('s:path[@marker-mid=$ref]', namespaces=ns,
                          ref=f'url(#{marker.get("id")})')
        assert len(uses) == 1
        path = uses[0]
        assert set(path.attrib) == {"fill", "marker-mid", "d"} and path.get("fill") == "none"
        commands = list(SVGPath(d=path.get("d")).absolute().as_cmd_seq())
        assert commands[0][0] == "M" and commands[-1][0] == "Z"
        assert len(commands[1:-1]) == 50 and all(c == "L" for c, _ in commands[1:-1])
        group = etree.Element("{http://www.w3.org/2000/svg}g")
        for _, (x, y) in commands[1:-1]:
            star = etree.SubElement(group, "{http://www.w3.org/2000/svg}g",
                                    transform=f"translate({x},{y})")
            for child in marker:
                star.append(deepcopy(child))
        root.replace(path, group)
        root.remove(marker)


def normalize_svg(text, width, height, menu=False):
    """Keep NanoSVG-supported geometry; flatten only flags needing clipping.

    Full picosvg conversion expands strokes and transforms unnecessarily for
    most flags. Keep these, shapes, fill rules and compact path coordinates
    intact. Clipped flags and menu artwork retain the full compatibility pass.
    Unknown elements also go through that pass, which rejects unsupported SVG.
    """
    if menu:
        text = text.replace("currentColor", "#212529")
    # Some flag-icons stroke widths use points; picosvg expects SVG user units.
    text = re.sub(r'"([0-9.]+)pt"', lambda m: f'"{float(m[1]) * 4 / 3:g}"', text)
    svg = SVG.fromstring(text)
    expand_star_markers(svg)
    svg.apply_style_attributes(inplace=True)
    svg.resolve_use(inplace=True)
    supported = {"svg", "g", "defs", "path", "rect", "circle", "ellipse",
                 "line", "polyline", "polygon", "linearGradient", "radialGradient", "stop"}
    flatten = menu or any(
        etree.QName(el).localname not in supported or
        el.get("clip-path", "none") != "none"
        for el in svg.svg_root.iter()
    )
    if flatten:
        svg = svg.topicosvg()
    else:
        # References have been expanded. Only gradient definitions are still
        # needed; retain their IDs and hrefs so NanoSVG can resolve them.
        for defs in svg.svg_root.findall(".//{http://www.w3.org/2000/svg}defs"):
            for child in list(defs):
                if etree.QName(child).localname not in {"linearGradient", "radialGradient"}:
                    defs.remove(child)
    if not menu:
        for el in svg.svg_root.iter():
            if "d" in el.attrib:
                path = SVGPath(d=el.get("d"))
                # Serialize explicit arc arguments: packed arc flags emitted
                # by some minifiers are misread by wx 3.2's NanoSVG.
                # Iteration preserves arcs and shorthand. as_cmd_seq() would
                # expand both and turn arcs into cubic curves, growing paths.
                candidates = [SVGPath.from_commands(path).d]
                if flatten:
                    # The full pass already rounded absolute coordinates to
                    # 3 decimals. Relative differences need no more precision;
                    # round only floating-point subtraction noise here.
                    candidates.append(path.relative().round_floats(3).d)
                candidates = [re.sub(r"\s+(?=[a-zA-Z])", "", d) for d in candidates]
                el.set("d", min(candidates, key=len))
            el.text = el.tail = None
    svg.svg_root.set("width", str(width))
    svg.svg_root.set("height", str(height))
    svg.svg_root.attrib.pop("class", None)
    return svg.tostring() + "\n"


def generate(source, destination, width, height, menu=False):
    destination.write_text(normalize_svg(source.read_text(encoding="utf-8"),
                                         width, height, menu), encoding="utf-8")
    subprocess.run(["rsvg-convert", "--width", str(width), "--height", str(height),
                    "--output", str(destination.with_suffix(".png")), str(destination)], check=True)


def optimize_png(path):
    """Recompress PNG image data without changing pixels or ancillary chunks."""
    data = path.read_bytes()
    chunks, offset = [], 8
    while offset < len(data):
        length = struct.unpack_from(">I", data, offset)[0]
        kind = data[offset + 4:offset + 8]
        chunks.append((kind, data[offset + 8:offset + 8 + length]))
        offset += length + 12
    compressed = zlib.compress(zlib.decompress(b"".join(payload for kind, payload in chunks if kind == b"IDAT")), 9)
    output, written = bytearray(data[:8]), False
    for kind, payload in chunks:
        if kind == b"IDAT":
            if written:
                continue
            payload, written = compressed, True
        output += struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))
    path.write_bytes(output)


def write_png(path, width, height, rgba):
    """Write 8-bit RGBA pixels with filter type 0, as the maintenance test reads them."""
    stride = width * 4
    raw = b"".join(b"\0" + rgba[y * stride:(y + 1) * stride] for y in range(height))

    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))

    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def read_ico_frame(path, size, bpp):
    """Decode the size x size BMP frame of an .ico with the given bit depth to RGBA."""
    data = path.read_bytes()
    _, kind, count = struct.unpack_from("<HHH", data)
    assert kind == 1
    for index in range(count):
        offset = struct.unpack_from("<I", data, 6 + 16 * index + 12)[0]
        if data[offset:offset + 4] == b"\x89PNG":
            continue
        header, width, height, _, depth, compression, _, _, _, used = struct.unpack_from("<IiiHHIIiiI", data, offset)
        if (width, height, depth) != (size, 2 * size, bpp):
            continue
        assert compression == 0
        palette = offset + header
        pixels = palette + (4 * (used or 1 << depth) if depth <= 8 else 0)
        stride = (size * depth + 31) // 32 * 4
        mask = pixels + stride * size
        mask_stride = (size + 31) // 32 * 4
        rgba = bytearray()
        # BMP rows run bottom-up; a set AND-mask bit is transparent.
        for y in range(size - 1, -1, -1):
            row = pixels + y * stride
            for x in range(size):
                hidden = data[mask + y * mask_stride + x // 8] >> (7 - x % 8) & 1
                if depth == 32:
                    b, g, r, a = data[row + 4 * x:row + 4 * x + 4]
                elif depth == 8:
                    b, g, r = data[palette + 4 * data[row + x]:palette + 4 * data[row + x] + 3]
                    a = 0 if hidden else 255
                else:
                    raise ValueError(f"unsupported bit depth {depth}")
                rgba += bytes((r, g, b, a))
        return bytes(rgba)
    raise ValueError(f"{path.name} has no {size}px {bpp}-bit frame")


def replicate(rgba, size, factor):
    """Enlarge pixel art by repeating each pixel, keeping its hard edges."""
    out = bytearray()
    for y in range(size * factor):
        for x in range(size * factor):
            start = ((y // factor) * size + x // factor) * 4
            out += rgba[start:start + 4]
    return bytes(out)


# Client mascots taken from each client's own icon: the frame size per density,
# or None to enlarge the 16px frame where the icon has no matching drawing.
CLIENT_ICONS = {
    "client_emule": ("emule/Mule.ico", 32, (16, 32)),
    "client_lphant": ("lphant/App.ico", 8, (16, None, None)),
    "client_shareaza": ("shareaza/Shareaza.ico", 32, (16, 32, 48)),
}


def client_icon_pixels(art_id):
    """Yield (suffix, size, RGBA) for each density of a client icon."""
    source, bpp, frames = CLIENT_ICONS[art_id]
    base = read_ico_frame(ROOT / "vendor" / source, 16, bpp)
    for scale, frame in enumerate(frames, 1):
        suffix = "" if scale == 1 else f"@{scale}x"
        if frame is None:
            yield suffix, 16 * scale, replicate(base, 16, scale)
        else:
            yield suffix, frame, read_ico_frame(ROOT / "vendor" / source, frame, bpp)


def mldonkey_camel():
    """MLDonkey's splash screen cropped to its camel's head, as a 16px SVG."""
    tree = etree.parse(str(ROOT / "vendor/mldonkey/splash_screen.svg"))
    root = tree.getroot()
    scene = root.xpath("//*[@id='g1038']")[0]
    for child in list(scene):
        if child.get("id") != "g793":
            scene.remove(child)
    # The splash screen's 688x400 viewport would stretch the square crop.
    root.set("viewBox", "99 87 94 94")
    root.set("width", "16")
    root.set("height", "16")
    return tree


def generate_client_icons():
    for art_id in CLIENT_ICONS:
        for suffix, size, rgba in client_icon_pixels(art_id):
            write_png(ROOT / f"{art_id}{suffix}.png", size, size, rgba)
    # The 1x camel is drawn by hand, because a 16px render loses the eye.
    from tempfile import TemporaryDirectory
    with TemporaryDirectory() as directory:
        camel = Path(directory) / "camel.svg"
        mldonkey_camel().write(str(camel))
        for scale in (2, 3):
            destination = ROOT / f"client_mldonkey@{scale}x.png"
            subprocess.run(["rsvg-convert", "--width", str(16 * scale), "--height", str(16 * scale),
                            "--output", str(destination), str(camel)], check=True)
            optimize_png(destination)


def generate_flag(source, code):
    for scale in (1, 2, 3):
        suffix = "" if scale == 1 else f"@{scale}x"
        destination = ROOT / "flags" / f"{code}{suffix}.png"
        subprocess.run(["rsvg-convert", "--width", str(16 * scale), "--height", str(12 * scale),
                        "--output", str(destination), str(source)], check=True)
        optimize_png(destination)


def main():
    for source in sorted((ROOT / "vendor/bootstrap-icons").glob("*.svg")):
        generate(source, ROOT / ("menu_" + source.stem.replace("-", "_") + ".svg"), 16, 16, True)
    for source in sorted((ROOT / "vendor/flag-icons").glob("*.svg")):
        generate_flag(source, source.stem)
    # The public-domain legacy flags have no vector original. Use the padded
    # 16x12 PNG as source, preserving transparent padding at every density.
    import base64
    from tempfile import TemporaryDirectory
    for code in ("an", "unknown"):
        source = ROOT / "flags" / f"{code}.png"
        with TemporaryDirectory() as directory:
            wrapper = Path(directory) / "legacy.svg"
            encoded = base64.b64encode(source.read_bytes()).decode("ascii")
            wrapper.write_text(f'<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="16" height="12" viewBox="0 0 16 12"><image width="16" height="12" xlink:href="data:image/png;base64,{encoded}"/></svg>')
            # Preserve the existing 1x bytes; only generate density siblings.
            original = source.read_bytes()
            generate_flag(wrapper, code)
            source.write_bytes(original)
    for obsolete in (ROOT / "flags").glob("*.svg"):
        obsolete.unlink()
    generate_client_icons()


if __name__ == "__main__":
    main()
