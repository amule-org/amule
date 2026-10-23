#!/usr/bin/env python3
"""Test the installed media helper and daemon from a relocated package.

No build-tree libraries or ffmpeg are used. --appimage extracts the actual
artifact and enters through AppRun; --flatpak runs installed binaries inside
the sandbox. Other packages provide --bin-dir and --licenses explicitly.
"""
import argparse
import io
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time
import wave


NOTICES = ('MediaInfoLib.html', 'ZenLib.txt', 'AES.txt', 'TinyXML2.txt',
           'SHA-Gladman.txt', 'HMAC-Gladman.txt', 'TFSXML.txt', 'fmt.txt')
TITLE = 'Runtime 艺术家 🎵 "quoted" \\ path'


def make_wave(path):
    with wave.open(str(path), 'wb') as audio:
        audio.setparams((1, 2, 48000, 0, 'NONE', 'not compressed'))
        audio.writeframes(bytes(48000 * 2 * 3))
    def synchsafe(n):
        return bytes((n >> 21 & 127, n >> 14 & 127, n >> 7 & 127, n & 127))
    frames = b''
    for key, value in ((b'TIT2', TITLE), (b'TPE1', 'Runtime Artist'), (b'TALB', 'Runtime Album')):
        value = b'\x03' + value.encode('utf-8')
        frames += key + synchsafe(len(value)) + b'\0\0' + value
    tag = b'ID3\x04\0\0' + synchsafe(len(frames)) + frames
    data = bytearray(path.read_bytes() + b'id3 ' + struct.pack('<I', len(tag)) + tag
                     + b'\0' * (len(tag) % 2))
    struct.pack_into('<I', data, 4, len(data) - 8)
    path.write_bytes(data)


def known_files(path):
    """Read the daemon's persisted known.met, preserving numeric media tags."""
    data = io.BytesIO(path.read_bytes())
    def read(n):
        value = data.read(n)
        if len(value) != n:
            raise ValueError('incomplete known.met')
        return value
    def number(n):
        return int.from_bytes(read(n), 'little')
    if number(1) not in (0x0e, 0x0f):
        raise ValueError('unknown known.met header')
    result = []
    count = number(4)
    if count > 100:
        raise ValueError('unexpected fixture record count')
    for _ in range(count):
        read(20)  # modification time and MD4
        read(16 * number(2))
        tags = {}
        for _ in range(number(4)):
            kind = number(1)
            if kind & 0x80:
                name = read(1)
                kind &= 0x7f
            else:
                name = read(number(2))
            if kind == 2:
                value = read(number(2)).decode('utf-8-sig')
            elif kind in (3, 8, 9, 11):
                value = number({3: 4, 8: 2, 9: 1, 11: 8}[kind])
            elif kind == 1:
                value = read(16)
            else:
                raise ValueError('unexpected fixture tag type: ' + str(kind))
            tags[name] = value
        result.append(tags)
    return result


def free_port():
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        return listener.getsockname()[1]


def run(args, root):
    env = dict(os.environ)
    env.pop('LD_PRELOAD', None)
    bin_dir = Path(args.bin_dir).resolve() if args.bin_dir else None
    licenses = Path(args.licenses).resolve() if args.licenses else None
    appdir = None
    if args.appimage:
        image = Path(args.appimage).resolve()
        image.chmod(image.stat().st_mode | 0o100)
        subprocess.run([str(image), '--appimage-extract'], cwd=root, env=env,
                       stdout=subprocess.DEVNULL, check=True, timeout=120)
        appdir = root / 'squashfs-root'
        licenses = appdir / 'usr/share/licenses/amule-media-probe'
    if licenses is None:
        raise AssertionError('--licenses is required for this package')
    for name in NOTICES:
        assert (licenses / name).is_file(), 'missing packaged notice: ' + str(licenses / name)
    if args.flatpak:
        assert bin_dir is None, '--flatpak does not use a host bin directory'
    elif not appdir:
        assert bin_dir and bin_dir.is_dir(), '--bin-dir must point into the package'

    def command(binary):
        child_env = dict(env)
        if args.flatpak:
            return ['flatpak', 'run', '--filesystem=' + str(root),
                    '--command=/app/bin/' + binary, args.flatpak], child_env
        if appdir:
            child_env['ARGV0'] = binary
            return [str(appdir / 'AppRun')], child_env
        extension = '.exe' if os.name == 'nt' else ''
        path = bin_dir / (binary + extension)
        assert path.is_file(), 'missing packaged binary: ' + str(path)
        return (['arch', '-' + args.arch, str(path)] if args.arch else [str(path)]), child_env

    helper, child_env = command('amule-media-probe')
    version = subprocess.run(helper + ['--version'], env=child_env, capture_output=True,
                             text=True, timeout=30)
    assert version.returncode == 0, (version.returncode, version.stdout, version.stderr)
    assert 'amule-media-probe 1;' in version.stdout, version.stdout + version.stderr
    assert 'v26.05' in version.stdout, 'package did not use pinned MediaInfoLib: ' + version.stdout
    print('PASS packaged helper version:', version.stdout.strip(), flush=True)
    incoming = root / 'Incoming'
    incoming.mkdir()
    (root / 'Temp').mkdir()
    media = incoming / '媒体 fixture.wav'
    make_wave(media)
    with media.open('rb') as source:
        reply = subprocess.run(helper + ['--json'], stdin=source, env=child_env,
                               capture_output=True, check=True, timeout=30)
    result = json.loads(reply.stdout)
    for key, expected in (('length_seconds', 3), ('bitrate_kbps', 768),
                          ('codec', 'pcm_s16le'), ('artist', 'Runtime Artist'),
                          ('album', 'Runtime Album'), ('title', TITLE)):
        assert result[key] == expected, (key, expected, result)
    with media.open('rb') as source:
        reply = subprocess.run(helper, stdin=source, env=child_env,
                               capture_output=True, check=True, timeout=30)
    lines = reply.stdout.decode('utf-8').splitlines()
    assert len(lines) == 9 and lines[0] == 'AMMP 1' and lines[-1] == 'END', reply.stdout
    assert b'\r' not in reply.stdout and reply.stdout.endswith(b'END\n'), reply.stdout
    print('PASS packaged helper: duration, codec, tags, Unicode and exact framing', flush=True)
    broken = incoming / 'broken.wav'
    broken.write_bytes(b'not a media file')
    for payload in (b'', b'not a media file'):
        broken.write_bytes(payload)
        with broken.open('rb') as source:
            reply = subprocess.run(helper, stdin=source, env=child_env,
                                   capture_output=True, timeout=30)
        assert reply.returncode in (10, 11), (reply.returncode, reply.stderr)
        assert not reply.stdout, reply.stdout
    print('PASS packaged helper rejects empty and malformed media', flush=True)
    # A native Windows config uses backslashes; wxFileConfig treats them as
    # escapes. Forward slashes are accepted on every platform.
    config_root = root.as_posix()
    (root / 'amule.conf').write_text(f'''[eMule]
Nick=packaged-media-runtime
FirstRunWizardDone=1
Port={free_port()}
Address=127.0.0.1
UDPEnable=0
ConnectToKad=0
ConnectToED2K=0
NewVersionCheck=0
GeoIPEnabled=0
Reconnect=0
Serverlist=0
Ed2kServersUrl=
KadNodesUrl=
Language=en_US
IncomingDir={config_root}/Incoming
TempDir={config_root}/Temp
[ExternalConnect]
AcceptExternalConnections=1
ECAddress=127.0.0.1
ECPort={free_port()}
ECPassword={hashlib.md5(b'package-runtime').hexdigest()}
[MediaMetadata]
Enabled=1
''', encoding='utf-8')
    daemon, child_env = command('amuled')
    with (root / 'console.log').open('w', encoding='utf-8') as console:
        proc = subprocess.Popen(daemon + ['-c', str(root)],
                                env=child_env, stdout=console, stderr=console)
        try:
            deadline = time.monotonic() + 90
            while True:
                assert proc.poll() is None, 'packaged daemon exited before metadata was saved'
                try:
                    records = known_files(root / 'known.met')
                    good = next((r for r in records if r.get(b'\xd5') == 'pcm_s16le'), None)
                    bad = next((r for r in records if r.get(b'\x57') == 2), None)
                except (OSError, ValueError):
                    good = bad = None
                if good and bad:
                    assert good[b'\xd3'] == 3 and good[b'\xd4'] == 768, good
                    assert good[b'\xd0'] == 'Runtime Artist', good
                    assert good[b'\xd1'] == 'Runtime Album', good
                    assert good[b'\xd2'] == TITLE, good
                    break
                if time.monotonic() >= deadline:
                    raise AssertionError('packaged daemon did not persist metadata and marker 2')
                time.sleep(0.25)
        finally:
            if proc.poll() is None:
                proc.terminate()
            try:
                proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=15)
    print('PASS packaged daemon: sibling-helper discovery, metadata persistence and failure marker 2', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bin-dir')
    parser.add_argument('--licenses')
    parser.add_argument('--appimage')
    parser.add_argument('--flatpak')
    parser.add_argument('--arch', choices=('arm64', 'x86_64'))
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='amule packaged runtime ') as scratch:
        root = Path(scratch).resolve()
        try:
            run(args, root)
        except BaseException:
            for name in ('console.log', 'logfile'):
                path = root / name
                if path.is_file():
                    print('\n--- ' + name + ' ---\n' + path.read_text(errors='replace')[-12000:], flush=True)
            raise


if __name__ == '__main__':
    main()
