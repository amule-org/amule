#!/usr/bin/env python3
"""
Generate a C translation unit that embeds every PNG icon under this
directory tree (plus each icon's optional same-name SVG twin) into a
single static lookup table.

Usage:  embed_icons.py <icons_dir> <output_c_file>

The emitted file declares the following C-API (see icon_data.h for the
matching public declarations):

    static const unsigned char icon_<name>_png[] = { 0x89, 0x50, ... };
    static const unsigned char icon_<name>_svg[] = { 0x3c, 0x73, ... };
    ... one png array per icon, plus an svg array when a twin exists ...

    static const struct AMuleIconEntry icons[] = {
        { "amule", icon_amule_png, sizeof icon_amule_png, 0, 0 },
        { "toolbar_search", icon_toolbar_search_png,
          sizeof icon_toolbar_search_png, icon_toolbar_search_svg,
          sizeof icon_toolbar_search_svg },
        ...
    };

    const struct AMuleIconEntry *amule_find_icon(const char *name);
    const struct AMuleIconEntry *amule_get_all_icons(int *count);

Icons in <icons_dir>/<name>.png get the art-id "<name>".
Icons in <icons_dir>/flags/<code>.png get the art-id "flag_<code>".

A <name>.svg next to <name>.png is embedded as the icon's vector twin;
CamuleArtProvider renders every request for the icon from it on wx
builds with SVG (NanoSVG) support.  The PNG stays mandatory: it is the
raster fallback and defines the icon's natural size, so an .svg without
a matching .png is an error.
"""

import re
import sys
from pathlib import Path


def sanitise(name: str) -> str:
    """Convert an icon name to a C identifier component."""
    return name.replace("-", "_").replace(".", "_")


def svg_twin(png_path: Path):
    """Return the icon's .svg twin path, or None when it has none."""
    svg = png_path.with_suffix(".svg")
    return svg if svg.is_file() else None


def collect_icons(icons_dir: Path):
    """
    Yield (art_id, c_ident, png_path, svg_path-or-None) for every .png
    under icons_dir.

    Files directly under icons_dir use their stem as the art id
    ("foo.png" -> art_id "foo").  Files under icons_dir/flags/ get the
    "flag_" prefix ("us.png" -> art_id "flag_us").
    """
    entries = []

    # Top-level icons (everything except the flags/ subdir).
    for path in sorted(icons_dir.glob("*.png")):
        stem = path.stem
        entries.append((stem, sanitise(stem), path, svg_twin(path)))

    # Country flags.
    flags_dir = icons_dir / "flags"
    if flags_dir.is_dir():
        for path in sorted(flags_dir.glob("*.png")):
            stem = path.stem
            art_id = f"flag_{stem}"
            entries.append((art_id, sanitise(art_id), path, svg_twin(path)))

    return entries


def find_orphan_svgs(icons_dir: Path, entries):
    """List .svg files that have no .png twin (the PNG is mandatory)."""
    known = {svg for _art_id, _c_ident, _png, svg in entries if svg is not None}
    search_dirs = [icons_dir]
    flags_dir = icons_dir / "flags"
    if flags_dir.is_dir():
        search_dirs.append(flags_dir)

    orphans = []
    for directory in search_dirs:
        for svg in sorted(directory.glob("*.svg")):
            if svg not in known:
                orphans.append(svg)
    return orphans


_NUMBER = re.compile(r"[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?")


def count_packed_arc_flags(svg_path: Path) -> int:
    """
    Count arc commands whose flags are packed against the next value, e.g.
    "a7 7 0 00-1.4-1.1". Valid SVG, but wx 3.2's NanoSVG reads "00" as one
    number, shifts the remaining arguments and fills a large wrong area.
    """
    packed = 0
    text = svg_path.read_text(encoding="utf-8")
    for path_data in re.findall(r'\bd="([^"]*)"', text):
        for args in re.findall(r"[aA]([^A-Za-z]*)", path_data):
            i, pos = 0, 0
            while True:
                while i < len(args) and args[i] in " ,\t\r\n":
                    i += 1
                if i >= len(args):
                    break
                # Arc arguments repeat as rx ry rotation large-arc sweep x y.
                if pos % 7 in (3, 4):
                    if i + 1 < len(args) and args[i + 1] not in " ,\t\r\n":
                        packed += 1
                        break
                    i += 1
                else:
                    match = _NUMBER.match(args, i)
                    if match is None:
                        break
                    i = match.end()
                pos += 1
    return packed


def emit_byte_array(chunks, c_name: str, data: bytes):
    chunks.append(f"static const unsigned char {c_name}[] = {{\n")
    # 16 bytes per line, hex-formatted.
    line = []
    for i, b in enumerate(data):
        line.append(f"0x{b:02x}")
        if (i + 1) % 16 == 0:
            chunks.append("\t" + ", ".join(line) + ",\n")
            line = []
    if line:
        chunks.append("\t" + ", ".join(line) + "\n")
    chunks.append("};\n")
    chunks.append("\n")


def emit(out_path: Path, entries):
    chunks = []
    # The byte arrays are laid out 16 values per line; clang-format would
    # otherwise reflow every array in the file, so opt the whole TU out.
    # This marker also shields the checked-in src/icons/icon_data.c from
    # the clang-format CI check (which covers all of src/).
    chunks.append("// clang-format off\n")
    chunks.append("/*\n")
    chunks.append(" * Auto-generated by src/icons/embed_icons.py -- do not edit.\n")
    chunks.append(" *\n")
    chunks.append(" * Embeds every PNG icon under src/icons/ (and each icon's\n")
    chunks.append(" * optional SVG twin) as a static byte array, exposes them to\n")
    chunks.append(" * C++ via the C-API declared in icon_data.h.\n")
    chunks.append(" */\n")
    chunks.append("\n")
    chunks.append('#include "icon_data.h"\n')
    chunks.append("\n")

    # Per-icon byte arrays.
    for _art_id, c_ident, png_path, svg_path in entries:
        emit_byte_array(chunks, f"icon_{c_ident}_png", png_path.read_bytes())
        if svg_path is not None:
            emit_byte_array(chunks, f"icon_{c_ident}_svg", svg_path.read_bytes())

    # Lookup table.
    chunks.append("static const struct AMuleIconEntry icons[] = {\n")
    for art_id, c_ident, _png_path, svg_path in entries:
        if svg_path is not None:
            chunks.append(f'\t{{ "{art_id}", icon_{c_ident}_png,\n')
            chunks.append(f"\t\tsizeof icon_{c_ident}_png, icon_{c_ident}_svg,\n")
            chunks.append(f"\t\tsizeof icon_{c_ident}_svg }},\n")
        else:
            chunks.append(
                f'\t{{ "{art_id}", icon_{c_ident}_png, sizeof icon_{c_ident}_png, 0, 0 }},\n'
            )
    chunks.append("};\n")
    chunks.append("\n")
    chunks.append(
        "static const int icons_count = (int) (sizeof icons / sizeof icons[0]);\n"
    )
    chunks.append("\n")

    # API.
    chunks.append("#include <string.h>\n")
    chunks.append("\n")
    chunks.append(
        "const struct AMuleIconEntry *amule_find_icon(const char *name)\n"
    )
    chunks.append("{\n")
    chunks.append("\tint i;\n")
    chunks.append("\tfor (i = 0; i < icons_count; ++i) {\n")
    chunks.append("\t\tif (strcmp(icons[i].name, name) == 0) {\n")
    chunks.append("\t\t\treturn &icons[i];\n")
    chunks.append("\t\t}\n")
    chunks.append("\t}\n")
    chunks.append("\treturn (const struct AMuleIconEntry *) 0;\n")
    chunks.append("}\n")
    chunks.append("\n")
    chunks.append(
        "const struct AMuleIconEntry *amule_get_all_icons(int *count)\n"
    )
    chunks.append("{\n")
    chunks.append("\tif (count) {\n")
    chunks.append("\t\t*count = icons_count;\n")
    chunks.append("\t}\n")
    chunks.append("\treturn icons;\n")
    chunks.append("}\n")

    out_path.parent.mkdir(parents=True, exist_ok=True)
    # Force UTF-8: the header comment contains a non-ASCII em-dash, and
    # Path.write_text() would otherwise use the locale encoding (cp1252
    # on Windows/mingw CI), corrupting it or raising UnicodeEncodeError.
    out_path.write_text("".join(chunks), encoding="utf-8")


def main(argv):
    if len(argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2

    icons_dir = Path(argv[1]).resolve()
    out_path = Path(argv[2]).resolve()

    if not icons_dir.is_dir():
        print(f"error: {icons_dir} is not a directory", file=sys.stderr)
        return 1

    entries = collect_icons(icons_dir)
    if not entries:
        print(f"error: no .png files found under {icons_dir}", file=sys.stderr)
        return 1

    orphans = find_orphan_svgs(icons_dir, entries)
    if orphans:
        for svg in orphans:
            print(f"error: {svg} has no matching .png fallback", file=sys.stderr)
        return 1

    packed = False
    for _art_id, _c_ident, _png, svg in entries:
        if svg is not None and count_packed_arc_flags(svg):
            print(f"error: {svg} packs arc flags against the next value (e.g. 'a7 7 0 00-1.4'); "
                  "wx 3.2 mis-renders that. Separate them: 'a7 7 0 0 0 -1.4'.", file=sys.stderr)
            packed = True
    if packed:
        return 1

    # A duplicate art id, or a duplicate C identifier (distinct names that
    # collide once sanitise() folds '-'/'.' to '_' -- e.g. "foo-bar" vs
    # "foo_bar", or a top-level "flag_x.png" shadowing "flags/x.png"), would
    # emit two byte arrays with the same symbol (a C redefinition) and make
    # amule_find_icon() shadow one of the icons. The current icon set has no
    # such clash; guard so a future rename fails loudly here instead of with
    # an opaque compiler error or a silently missing icon.
    seen_art, seen_ident, collision = {}, {}, False
    for art_id, c_ident, png_path, _svg in entries:
        if art_id in seen_art:
            print(f"error: art id '{art_id}' ({png_path}) collides with {seen_art[art_id]}", file=sys.stderr)
            collision = True
        else:
            seen_art[art_id] = png_path
        if c_ident in seen_ident:
            print(f"error: C identifier 'icon_{c_ident}' ({png_path}) collides with "
                  f"{seen_ident[c_ident]}", file=sys.stderr)
            collision = True
        else:
            seen_ident[c_ident] = png_path
    if collision:
        return 1

    emit(out_path, entries)
    n_svg = sum(1 for e in entries if e[3] is not None)
    print(f"emitted {len(entries)} icons ({n_svg} with SVG) -> {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
