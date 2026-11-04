#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise source pin updates through the real builder, without compiling FFmpeg."""
import hashlib
import importlib.util
import io
import re
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
                if Path(command[0]).name.startswith('bash'):
                    configured.append(Path(command[1]).read_text())
                    (cwd / 'config.h').write_text('/* fixture configuration */')
                return subprocess.CompletedProcess(command, 0)

            for version, marker in zip(versions, ('old source', 'new source')):
                archive_name = 'ffmpeg-' + version + '.tar.xz'
                buffer = io.BytesIO()
                with tarfile.open(fileobj=buffer, mode='w:xz') as tar:
                    for name in ('configure', 'COPYING.LGPLv2.1', 'LICENSE.md'):
                        entry = tarfile.TarInfo('fixture/' + name)
                        payload = marker.encode()
                        entry.size = len(payload)
                        tar.addfile(entry, io.BytesIO(payload))
                archive_bytes = buffer.getvalue()
                pin = dict(FFMPEG_VERSION=version, FFMPEG_TARBALL_URL='https://example.invalid/source',
                           FFMPEG_SHA256=hashlib.sha256(archive_bytes).hexdigest())
                def download(url, **kwargs):
                    downloads.append(url)
                    return io.BytesIO(archive_bytes)

                opener = mock.Mock()
                opener.open.side_effect = download
                with mock.patch.object(BUILDER.urllib.request, 'build_opener', return_value=opener), \
                     mock.patch.object(BUILDER, 'pins', return_value=pin), \
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

    def test_changed_compiler_flags_reconfigure_the_same_source(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            work, prefix, tools = root / 'work', root / 'prefix', root / 'tools'
            tools.mkdir()
            for name in ('bash', 'make', 'bash.exe', 'make.exe'):
                (tools / name).touch()
            buffer = io.BytesIO()
            with tarfile.open(fileobj=buffer, mode='w:xz') as tar:
                for name in ('configure', 'COPYING.LGPLv2.1', 'LICENSE.md'):
                    entry = tarfile.TarInfo('fixture/' + name)
                    entry.size = 7
                    tar.addfile(entry, io.BytesIO(b'fixture'))
            archive = buffer.getvalue()
            pin = dict(FFMPEG_VERSION='1.0', FFMPEG_TARBALL_URL='https://example.invalid/source',
                       FFMPEG_SHA256=hashlib.sha256(archive).hexdigest())
            configured = []
            cleaned = []
            def run(command, cwd=None, **kwargs):
                if Path(command[0]).name.startswith('bash'):
                    configured.append(kwargs['env']['CFLAGS'])
                    (cwd / 'config.h').write_text('/* configured */')
                    (cwd / 'ffbuild').mkdir(exist_ok=True)
                    (cwd / 'ffbuild/config.mak').touch()
                elif command[1:] == ['distclean']:
                    cleaned.append(kwargs['env']['CFLAGS'])
                return subprocess.CompletedProcess(command, 0)
            opener = mock.Mock()
            opener.open.side_effect = lambda *a, **kw: io.BytesIO(archive)
            with mock.patch.object(BUILDER, 'pins', return_value=pin), \
                 mock.patch.object(BUILDER.urllib.request, 'build_opener', return_value=opener), \
                 mock.patch.object(BUILDER.shutil, 'which', side_effect=lambda name, **kw: str(tools / name)), \
                 mock.patch.object(BUILDER.subprocess, 'run', side_effect=run), \
                 mock.patch.object(sys, 'argv', ['build-ffmpeg.py', str(prefix), str(work)]):
                for flags in ('-DOLD_FLAGS=1', '-DNEW_FLAGS=1', '-DNEW_FLAGS=1'):
                    with mock.patch.dict(BUILDER.os.environ, {'CFLAGS': flags}):
                        BUILDER.main()
            # Observe actual configure execution and its environment. The final
            # unchanged invocation must reuse the same configured build.
            self.assertEqual(configured, ['-DOLD_FLAGS=1', '-DNEW_FLAGS=1'])
            self.assertEqual(cleaned, ['-DNEW_FLAGS=1'])

    def test_runtime_builder_and_flatpak_demuxers_match(self):
        root = Path(__file__).resolve().parents[2]
        main = (root / 'src/mediaprobe/main.cpp').read_text()
        declaration = re.search(r'const char \*formats\s*=\s*(.*?);', main, re.S).group(1)
        runtime = set(''.join(re.findall(r'"([^"]*)"', declaration)).split(','))
        # Runtime names include aliases; configure uses one demuxer name.
        aliases = dict(webm='matroska', mp4='mov', m4a='mov', **{'3gp': 'mov', '3g2': 'mov'},
                       mj2='mov', mpeg='mpegps')
        normalized = {aliases.get(name, name) for name in runtime}
        configured = set(BUILDER.DEMUXERS.split(','))
        flatpak = (root / 'packaging/linux/flatpak/org.amule.aMule.yaml.in').read_text()
        template = set(re.search(r'--enable-demuxer=([^\s]+)', flatpak).group(1).split(','))
        self.assertEqual(configured, normalized)
        self.assertEqual(configured, template)

    def test_version_update_reuses_no_old_sources(self):
        self.check_update(('1.0', '2.0'))

    def test_corrected_digest_under_same_version_reuses_no_old_sources(self):
        self.check_update(('1.0', '1.0'))


if __name__ == '__main__':
    unittest.main()
