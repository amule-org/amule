#!/usr/bin/env python3
"""Exercise source pin updates through the real builder, without compiling FFmpeg."""
import hashlib
import importlib.util
import io
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

SPEC = importlib.util.spec_from_file_location('media_builder', Path(__file__).with_name('build-ffmpeg.py'))
BUILDER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILDER)


class BuildRecipeTests(unittest.TestCase):
    def check_update(self, versions):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            work, prefix, tools = root / 'work', root / 'prefix', root / 'tools'
            work.mkdir()
            tools.mkdir()
            for name in ('bash', 'make', 'bash.exe', 'make.exe'):
                (tools / name).touch()
            configured = []
            downloads = []

            def run(command, cwd=None, **kwargs):
                if command[0] == 'gh':
                    downloads.append(command[-1])
                    kwargs['stdout'].write(archive_bytes)
                elif Path(command[0]).name.startswith('bash'):
                    configured.append(Path(command[1]).read_text())
                    (cwd / 'config.h').write_text('/* fixture configuration */')
                return subprocess.CompletedProcess(command, 0)

            for version, marker in zip(versions, ('old source', 'new source')):
                archive_name = 'FFmpeg-' + version + '.tar.gz'
                buffer = io.BytesIO()
                with tarfile.open(fileobj=buffer, mode='w:gz') as tar:
                    for name in ('configure', 'COPYING.LGPLv2.1', 'LICENSE.md'):
                        entry = tarfile.TarInfo('fixture/' + name)
                        payload = marker.encode()
                        entry.size = len(payload)
                        tar.addfile(entry, io.BytesIO(payload))
                archive_bytes = buffer.getvalue()
                pin = dict(FFMPEG_VERSION=version, FFMPEG_TARBALL_URL='https://example.invalid/source',
                           FFMPEG_SHA256=hashlib.sha256(archive_bytes).hexdigest())
                with mock.patch.object(BUILDER, 'pins', return_value=pin), \
                     mock.patch.object(sys, 'argv', ['build-ffmpeg.py', str(prefix), str(work)]), \
                     mock.patch.object(BUILDER.shutil, 'which', side_effect=lambda name, **kw: str(tools / name)), \
                     mock.patch.object(BUILDER.subprocess, 'run', side_effect=run):
                    BUILDER.main()
                installed = prefix / 'share/sources/amule-media-probe' / archive_name
                self.assertEqual(installed.read_bytes(), archive_bytes)
            # Inspect the configure script actually selected, not the directory naming scheme.
            self.assertEqual(configured, ['old source', 'new source'])
            self.assertEqual(len(downloads), 2)
            self.assertEqual((prefix / 'share/licenses/amule-media-probe/FFmpeg-LICENSE.md').read_text(),
                             'new source')

    def test_version_update_reuses_no_old_sources(self):
        self.check_update(('1.0', '2.0'))

    def test_corrected_digest_under_same_version_reuses_no_old_sources(self):
        self.check_update(('1.0', '1.0'))


if __name__ == '__main__':
    unittest.main()
