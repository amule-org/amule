#!/usr/bin/env python3
"""Maintenance tests; run with the same picosvg environment as regeneration."""

from pathlib import Path
from tempfile import TemporaryDirectory
import hashlib
import json
import unittest
import struct
import zlib

from lxml import etree
from picosvg.svg_types import SVGPath

from embed_icons import collect_icons, emit, count_packed_arc_flags
from regenerate_artwork import ROOT, CLIENT_ICONS, client_icon_pixels, mldonkey_camel, normalize_svg


def png_rgba(path):
    """Size and pixels of an 8-bit RGBA PNG written by write_png()."""
    data = path.read_bytes()
    width, height, depth, colour = struct.unpack('>IIBB', data[16:26])
    assert (depth, colour) == (8, 6), path.name
    idat, offset = b'', 8
    while offset < len(data):
        length = struct.unpack_from('>I', data, offset)[0]
        if data[offset + 4:offset + 8] == b'IDAT':
            idat += data[offset + 8:offset + 8 + length]
        offset += length + 12
    raw, stride = zlib.decompress(idat), width * 4 + 1
    assert all(raw[y * stride] == 0 for y in range(height)), path.name
    return width, height, b''.join(raw[y * stride + 1:(y + 1) * stride] for y in range(height))


class ArtworkConversionTest(unittest.TestCase):
    def normalize(self, content):
        return normalize_svg(
            '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 640 480">'
            + content + '</svg>', 16, 12)

    def test_vendored_checksums_and_distributed_licenses(self):
        manifest = json.loads((ROOT / 'vendor/manifest.json').read_text())
        notices = (ROOT.parent.parent / 'docs/THIRDPARTY.md').read_text()
        for project, source in manifest.items():
            for name, expected in source['files'].items():
                with self.subTest(project=project, file=name):
                    data = (ROOT / 'vendor' / project / name).read_bytes()
                    self.assertEqual(hashlib.sha256(data).hexdigest(), expected)
            license_text = (ROOT / 'vendor' / project / 'LICENSE').read_text().strip()
            self.assertIn(license_text, notices, project)

    def test_preserves_supported_geometry(self):
        result = self.normalize(
            '<g transform="translate(10 20)"><circle cx="20" cy="30" r="10" '
            'fill="none" stroke="red" stroke-width="2"/></g>')
        root = etree.fromstring(result.encode())
        self.assertEqual(len(root.findall('.//{*}circle')), 1)
        self.assertIn('stroke="red"', result)
        self.assertIn('transform="translate(10 20)"', result)
        self.assertNotIn('<path', result)

    def test_expands_references_but_keeps_transforms(self):
        result = self.normalize(
            '<defs><path id="p" d="M0 0h10v10z"/></defs>'
            '<use xmlns:xlink="http://www.w3.org/1999/xlink" '
            'xlink:href="#p" transform="translate(20 30)"/>')
        self.assertNotIn('<use', result)
        self.assertIn('transform=', result)
        self.assertEqual(len(etree.fromstring(result.encode()).findall('.//{*}path')), 1)

    def test_clipping_uses_full_compatibility_conversion(self):
        result = self.normalize(
            '<defs><clipPath id="c"><rect width="30" height="30"/></clipPath></defs>'
            '<circle cx="30" cy="30" r="20" clip-path="url(#c)"/>')
        self.assertNotIn('clip-path', result)
        self.assertNotIn('clipPath', result)
        self.assertIn('<path', result)

    def test_rejects_unsupported_artwork(self):
        with self.assertRaises(ValueError):
            self.normalize('<text x="0" y="20">Do not silently discard me</text>')

    def test_separates_arc_flags(self):
        result = self.normalize('<path d="M0 0a7 7 0 00-1.4-1.1z"/>')
        root = etree.fromstring(result.encode())
        commands = list(SVGPath(d=root.find('{*}path').get('d')))
        self.assertEqual(commands[1], ('a', (7, 7, 0, 0, 0, -1.4, -1.1)))
        with TemporaryDirectory() as directory:
            path = Path(directory) / 'arc.svg'
            path.write_text(result)
            self.assertEqual(count_packed_arc_flags(path), 0)

    def test_png_flag_dimensions_and_inventory(self):
        codes = [p.stem for p in (ROOT / 'vendor/flag-icons').glob('*.svg')] + ['an', 'unknown']
        self.assertEqual(len(list((ROOT / 'flags').glob('*.png'))), len(codes) * 3)
        for code in codes:
            for scale in (1, 2, 3):
                suffix = '' if scale == 1 else f'@{scale}x'
                path = ROOT / 'flags' / f'{code}{suffix}.png'
                with self.subTest(flag=path.name):
                    data = path.read_bytes()
                    self.assertEqual(data[:8], b'\x89PNG\r\n\x1a\n')
                    self.assertEqual(struct.unpack('>II', data[16:24]), (16 * scale, 12 * scale))

    def test_flags_are_embedded_and_embedding_is_deterministic(self):
        entries = collect_icons(ROOT)
        flags = [e for e in entries if e[0].startswith('flag_')]
        self.assertEqual(len(flags), 253)
        self.assertTrue(all(e[3] is None for e in flags))
        self.assertFalse(any('@' in e[0] for e in entries))
        with TemporaryDirectory() as directory:
            output = Path(directory) / 'icon_data.c'
            emit(output, entries)
            self.assertEqual(output.read_bytes(), (ROOT / 'icon_data.c').read_bytes())
            self.assertNotIn(b'zlib', output.read_bytes())
            self.assertIn(b'icon_flag_us_png2x', output.read_bytes())
            self.assertIn(b'icon_flag_us_png3x', output.read_bytes())

    def test_client_icons_are_the_vendored_frames(self):
        for art_id in CLIENT_ICONS:
            for suffix, size, expected in client_icon_pixels(art_id):
                with self.subTest(icon=art_id + suffix):
                    self.assertEqual(png_rgba(ROOT / f'{art_id}{suffix}.png'), (size, size, expected))

    def test_mldonkey_camel_crop_is_square(self):
        root = mldonkey_camel().getroot()
        _, _, width, height = root.get('viewBox').split()
        self.assertEqual(width, height)
        self.assertEqual(root.get('width'), root.get('height'))
        self.assertEqual([g.get('id') for g in root.xpath("//*[@id='g1038']/*")], ['g793'])

    def test_client_icon_densities_and_twins(self):
        entries = {entry[0]: entry for entry in collect_icons(ROOT)}
        # An SVG twin wins over every PNG density, so raster mascots must not have one.
        for art_id, densities, vector in (('client_amule', 1, True), ('client_xmule', 1, True),
                                          ('client_edonkeyhybrid', 1, True), ('client_emule', 2, False),
                                          ('client_lphant', 3, False), ('client_shareaza', 3, False),
                                          ('client_mldonkey', 3, False)):
            with self.subTest(icon=art_id):
                self.assertEqual(entries[art_id][3] is not None, vector)
                for scale in (1, 2, 3):
                    path = ROOT / (art_id + ('' if scale == 1 else f'@{scale}x') + '.png')
                    self.assertEqual(path.is_file(), scale <= densities)
                    if path.is_file():
                        self.assertEqual(struct.unpack('>II', path.read_bytes()[16:24]), (16 * scale, 16 * scale))

    def test_flag_raster_payload_and_originals(self):
        self.assertFalse(list((ROOT / 'flags').glob('*.svg')))
        self.assertEqual(len(list((ROOT / 'vendor/flag-icons').glob('*.svg'))), 251)
        self.assertLess(sum(p.stat().st_size for p in (ROOT / 'flags').glob('*.png')), 500_000)


if __name__ == '__main__':
    unittest.main()
