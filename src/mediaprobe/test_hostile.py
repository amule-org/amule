#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Hostile inputs: run this against full system FFmpeg as well as the bundle.

Linux strace observes actual file/network syscalls, including opens that don't
produce output. AMULE_REQUIRE_STRACE=1 makes absence an error in the system lane.
Other platforms exercise the verdict/framing tests without claiming syscall proof.
"""
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import unittest
import wave

HELPER = str(Path(sys.argv.pop(1)).resolve())
STRACE = shutil.which('strace') if sys.platform.startswith('linux') else None
if os.environ.get('AMULE_REQUIRE_STRACE') == '1' and not STRACE:
    raise SystemExit('strace is required in this lane')


class HostileTests(unittest.TestCase):
    def test_playlists_and_external_references(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            external = root / 'external-canary.wav'
            with wave.open(str(external), 'wb') as f:
                f.setparams((1, 2, 8000, 0, 'NONE', 'not compressed'))
                f.writeframes(bytes(32000))
            with socket.socket() as listener:
                listener.bind(('127.0.0.1', 0))
                listener.listen()
                listener.settimeout(0.1)
                url = 'http://127.0.0.1:%d/external-canary.wav' % listener.getsockname()[1]
                for name, payload in (
                    ('hls-local', '#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXTINF:2,\n' + external.as_posix() + '\n#EXT-X-ENDLIST\n'),
                    ('hls-http', '#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXTINF:2,\n' + url + '\n#EXT-X-ENDLIST\n'),
                    ('concat-local', "ffconcat version 1.0\nfile '" + external.as_posix() + "'\n"),
                    ('concat-http', "ffconcat version 1.0\nfile '" + url + "'\n"),
                    ('sdp', 'v=0\no=- 0 0 IN IP4 127.0.0.1\ns=hostile\nc=IN IP4 127.0.0.1\nt=0 0\nm=audio 45678 RTP/AVP 0\n'),
                    ('dash', '<?xml version="1.0"?><MPD xmlns="urn:mpeg:dash:schema:mpd:2011"><Period><AdaptationSet><Representation><BaseURL>' + url + '</BaseURL></Representation></AdaptationSet></Period></MPD>'),
                    ('hls-crypto', '#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXT-X-KEY:METHOD=AES-128,URI="' + url + '"\n#EXTINF:2,\n' + external.as_posix() + '\n#EXT-X-ENDLIST\n'),
                ):
                    # Misleading extension: only byte-level probing decides the format.
                    path = root / (name + '.mp4')
                    path.write_text(payload)
                    with self.subTest(name=name):
                        self.run_rejected(path, external, root)
                with self.assertRaises(socket.timeout):
                    listener.accept()

    @unittest.skipUnless(shutil.which('ffmpeg'), 'MOV reference fixture needs ffmpeg')
    def test_supported_mov_external_track_cannot_open_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            external = root / 'external-track-canary.mov'
            subprocess.run([shutil.which('ffmpeg'), '-v', 'error', '-nostdin', '-y',
                            '-f', 'lavfi', '-i', 'sine=sample_rate=8000:duration=1',
                            '-c:a', 'pcm_s16le', str(external)], check=True, timeout=20)
            filename = external.name.encode()[:63]
            path = external.as_posix().encode()
            # QuickTime AliasRecord: Pascal volume/name, directory levels, absolute path.
            alias = (bytes(10) + bytes(28) + bytes(12) + bytes([len(filename)]) +
                     filename.ljust(63, b'\0') + bytes(16) + struct.pack('>HH', 1, 1) +
                     bytes(16) + struct.pack('>HH', 2, len(path)) + path +
                     bytes(len(path) % 2) + struct.pack('>HH', 0xffff, 0))
            entry = struct.pack('>I4sI', len(alias) + 12, b'alis', 0) + alias
            replacement = struct.pack('>II', 0, 1) + entry

            def rewrite(data):
                result = b''
                offset = 0
                while offset < len(data):
                    size, kind = struct.unpack_from('>I4s', data, offset)
                    assert size >= 8 and offset + size <= len(data), (kind, size)
                    payload = data[offset + 8:offset + size]
                    if kind == b'dref':
                        payload = replacement
                    elif kind in (b'moov', b'trak', b'mdia', b'minf', b'dinf'):
                        payload = rewrite(payload)
                    result += struct.pack('>I4s', len(payload) + 8, kind) + payload
                    offset += size
                return result

            reference = root / 'reference.mov'
            reference.write_bytes(rewrite(external.read_bytes()))
            self.assertIn(b'alis', reference.read_bytes())
            self.assertIn(path, reference.read_bytes())
            # It may retain useful container metadata; access is the property under test.
            command = [HELPER]
            trace = root / 'mov.trace'
            if STRACE:
                command = [STRACE, '-f', '-qq', '-s', '4096', '-e', 'trace=%file,%network',
                           '-o', str(trace), *command]
            with reference.open('rb') as source:
                result = subprocess.run(command, stdin=source, capture_output=True, timeout=30)
            self.assertIn(result.returncode, (0, 10, 11), result.stderr)
            self.assertTrue(result.stdout.startswith(b'AMMP 1\n'))
            if STRACE:
                events = trace.read_text()
                self.assertNotIn(external.name, events)
                self.assertNotIn('socket(', events)
                self.assertNotIn('connect(', events)

    def run_rejected(self, path, external, root):
        command = [HELPER]
        trace = root / (path.stem + '.trace')
        if STRACE:
            command = [STRACE, '-f', '-qq', '-s', '4096', '-e', 'trace=%file,%network',
                       '-o', str(trace), *command]
        with path.open('rb') as source:
            result = subprocess.run(command, stdin=source, capture_output=True, timeout=30)
        self.assertIn(result.returncode, (10, 11), result.stderr)
        self.assertEqual(result.stdout, b'AMMP 1\n')
        if STRACE:
            events = trace.read_text()
            self.assertNotIn(external.name, events)
            self.assertNotIn('socket(', events)
            self.assertNotIn('connect(', events)


if __name__ == '__main__':
    unittest.main()
