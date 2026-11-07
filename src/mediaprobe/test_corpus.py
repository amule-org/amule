#!/usr/bin/env python3
"""Archive validation matters because fuzz inputs are deliberately untrusted."""
import hashlib
import gzip
import io
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch

import corpus


class CorpusTests(unittest.TestCase):
    def test_rejects_excessive_decompression_before_interpreting_tar_headers(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / 'bomb.tar.gz'
            archive.write_bytes(gzip.compress(b'\0' * 2048))
            with patch.object(corpus, 'MAX_TAR_BYTES', 1024):
                with self.assertRaisesRegex(ValueError, 'decompressed'):
                    corpus.unpack(archive, root / 'output')
            self.assertFalse((root / 'output').exists())

    def test_round_trip_is_reproducible_and_preserves_input_bytes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'source'
            source.mkdir()
            data = b'RIFF\0\xffhostile fixture'
            (source / 'original-name').write_bytes(data)
            (source / 'empty-input').write_bytes(b'')
            corpus.pack(source, root / 'one.tar.gz')
            corpus.pack(source, root / 'two.tar.gz')
            self.assertEqual((root / 'one.tar.gz').read_bytes(), (root / 'two.tar.gz').read_bytes())
            corpus.unpack(root / 'one.tar.gz', root / 'loaded')
            self.assertEqual((root / 'loaded' / hashlib.sha256(data).hexdigest()).read_bytes(), data)
            self.assertEqual((root / 'loaded' / hashlib.sha256(b'').hexdigest()).read_bytes(), b'')

    def test_rejects_traversal_links_oversized_inputs_and_wrong_hashes(self):
        for name, kind, size in (('../escape', tarfile.REGTYPE, 1),
                                 ('a' * 64, tarfile.SYMTYPE, 0),
                                 ('a' * 64, tarfile.REGTYPE, corpus.MAX_INPUT + 1),
                                 ('a' * 64, tarfile.REGTYPE, 1)):
            with self.subTest(name=name, kind=kind, size=size), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                archive = root / 'bad.tar.gz'
                with tarfile.open(archive, 'w:gz') as tar:
                    entry = tarfile.TarInfo(name)
                    entry.type, entry.size = kind, size
                    tar.addfile(entry, io.BytesIO(b'x' * size))
                with self.assertRaises(ValueError):
                    corpus.unpack(archive, root / 'output')
                self.assertFalse((root / 'output').exists())


if __name__ == '__main__':
    unittest.main()
