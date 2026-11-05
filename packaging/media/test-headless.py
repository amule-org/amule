#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check a minimal Linux helper's dependencies and run it in a clean base image.

Use an image matching the build distribution/architecture. Pull it beforehand;
the runtime check has no network and installs no packages.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile
import wave


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('helper', type=Path)
    parser.add_argument('--image', default='ubuntu:24.04')
    args = parser.parse_args()
    helper = args.helper.resolve()
    dynamic = subprocess.run(['readelf', '-d', str(helper)], check=True,
                             capture_output=True, text=True).stdout
    needed = set(re.findall(r'\(NEEDED\).*?\[([^\]]+)\]', dynamic))
    allowed = {'libc.so.6', 'libm.so.6', 'libz.so.1', 'libstdc++.so.6', 'libgcc_s.so.1'}
    if not needed or needed - allowed:
        raise AssertionError('unexpected minimal-helper dependencies: ' + repr(sorted(needed)))
    print('PASS ELF dependencies:', ', '.join(sorted(needed)), flush=True)
    with tempfile.TemporaryDirectory(prefix='amule-headless-') as temporary:
        root = Path(temporary)
        media = root / 'sample.wav'
        with wave.open(str(media), 'wb') as audio:
            audio.setparams((1, 2, 48000, 0, 'NONE', 'not compressed'))
            audio.writeframes(bytes(48000 * 2 * 3))
        (root / 'broken.wav').write_bytes(b'not media')
        command = ['docker', 'run', '--rm', '--pull=never', '--network=none',
                   '--mount', 'type=bind,src=' + str(helper) + ',dst=/probe,readonly',
                   '--mount', 'type=bind,src=' + str(root) + ',dst=/fixtures,readonly',
                   args.image, 'sh', '-ec']
        # Inspect the actual installed package set, including indirect libraries.
        packages = subprocess.run(command + [
            "dpkg-query -W -f='${binary:Package} ${db:Status-Status}\\n'"],
            check=True, capture_output=True, text=True, timeout=30).stdout
        unwanted = [line for line in packages.splitlines() if line.endswith(' installed') and
                    re.match(r'(ffmpeg|libavcodec|libavformat|libavutil|libx11|libxcb|libcairo|libpango|librsvg)', line)]
        if unwanted:
            raise AssertionError('base image contains media/UI libraries: ' + repr(unwanted))
        reply = subprocess.run(command + ['/probe --json < /fixtures/sample.wav'],
                               check=True, capture_output=True, text=True, timeout=30)
        result = json.loads(reply.stdout)
        for key, expected in (('length_seconds', 3), ('bitrate_kbps', 768), ('codec', 'pcm_s16le')):
            if result[key] != expected:
                raise AssertionError((key, expected, result))
        framed = subprocess.run(command + ['/probe < /fixtures/sample.wav'],
                                check=True, capture_output=True, timeout=30).stdout
        if not framed.startswith(b'AMMP 1\n') or not framed.endswith(b'END\n'):
            raise AssertionError(framed)
        rejected = subprocess.run(command + ['/probe < /fixtures/broken.wav'],
                                  capture_output=True, timeout=30)
        if rejected.returncode not in (10, 11) or rejected.stdout != b'AMMP 1\n':
            raise AssertionError((rejected.returncode, rejected.stdout, rejected.stderr))
    print('PASS clean headless runtime: WAV metadata, framing and malformed-file rejection; no added packages')


if __name__ == '__main__':
    main()
