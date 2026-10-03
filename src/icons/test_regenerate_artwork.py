#!/usr/bin/env python3
"""Maintenance tests; run with the same picosvg environment as regeneration."""

from pathlib import Path
from tempfile import TemporaryDirectory
import hashlib
import json
import unittest

from lxml import etree
from picosvg.svg_types import SVGPath

from embed_icons import count_packed_arc_flags
from regenerate_artwork import ROOT, normalize_svg


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

    def test_all_flags_remain_vector_with_bounded_payload(self):
        originals = sorted((ROOT / 'vendor/flag-icons').glob('*.svg'))
        generated = sorted((ROOT / 'flags').glob('*.svg'))
        self.assertEqual([p.name for p in originals], [p.name for p in generated])
        # Catch accidental reintroduction of the 7.8 MB blanket conversion.
        self.assertLess(sum(p.stat().st_size for p in generated), 3_500_000)
        for path in generated:
            with self.subTest(flag=path.name):
                self.assertEqual(count_packed_arc_flags(path), 0)


if __name__ == '__main__':
    unittest.main()
