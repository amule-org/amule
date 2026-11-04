#!/usr/bin/env python3
"""Build the pinned metadata-only FFmpeg; keep exact source and LGPL notices."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DEMUXERS = ('aac,ac3,aiff,amr,ape,asf,avi,dts,eac3,flac,flv,gsm,h264,hevc,'
            'matroska,mov,mp3,mpegps,mpegts,mpegvideo,ogg,wav,wv,tta')


def pins():
    values = {}
    for line in (ROOT / 'packaging/linux/versions.env').read_text().splitlines():
        match = re.match(r'(FFMPEG_\w+)=(.*)', line)
        if match:
            values[match[1]] = match[2]
    values['FFMPEG_TARBALL_URL'] = values['FFMPEG_TARBALL_URL'].replace(
        '${FFMPEG_VERSION}', values['FFMPEG_VERSION'])
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('prefix', nargs='?', default='/usr/local')
    parser.add_argument('work', nargs='?', default='/tmp/amule-media-deps')
    parser.add_argument('--source-only', action='store_true')
    args, cmake_options = parser.parse_known_args()
    prefix, work = Path(args.prefix).resolve(), Path(args.work).resolve()
    work.mkdir(parents=True, exist_ok=True)
    pin = pins()
    env = {k: v for k, v in os.environ.items()
           if k.lower() not in ('http_proxy', 'https_proxy', 'all_proxy', 'ld_preload')}
    archive = work / ('FFmpeg-' + pin['FFMPEG_VERSION'] + '.tar.gz')
    cached = ROOT / 'packaging/media/sources' / archive.name
    if not archive.exists() and cached.exists():
        shutil.copy2(cached, archive)
    if not archive.exists():
        partial = archive.with_suffix('.partial')
        with partial.open('wb') as output:
            subprocess.run(['gh', 'api', 'repos/FFmpeg/FFmpeg/tarball/n' + pin['FFMPEG_VERSION']],
                           stdout=output, env=env, check=True)
        partial.replace(archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != pin['FFMPEG_SHA256']:
        raise SystemExit('FFmpeg source checksum mismatch: ' + str(archive))
    source_dest = prefix if args.source_only else prefix / 'share/sources/amule-media-probe'
    source_dest.mkdir(parents=True, exist_ok=True)
    shutil.copy2(archive, source_dest / archive.name)
    if args.source_only:
        return
    # A pin update must never reuse the previous source/build tree. Include the
    # digest as well as version so correcting an archive under the same tag works.
    identity = pin['FFMPEG_VERSION'] + '-' + pin['FFMPEG_SHA256'][:16]
    source = work / ('ffmpeg-' + identity)
    if not source.exists():
        with tempfile.TemporaryDirectory(prefix='ffmpeg-extract-', dir=work) as temporary:
            extracted = Path(temporary) / 'source'
            extracted.mkdir()
            with tarfile.open(archive) as tar:
                # Older Python versions have no tar extraction filters.
                for entry in tar.getmembers():
                    parts = Path(entry.name).parts[1:]
                    if not parts:
                        continue
                    if '..' in parts or entry.issym() or entry.islnk():
                        raise SystemExit('Unexpected archive member: ' + entry.name)
                    entry.name = Path(*parts).as_posix()
                    tar.extract(entry, extracted)
            extracted.replace(source)
    build = work / ('ffmpeg-build-' + identity)
    build.mkdir(exist_ok=True)
    configure = ['--prefix=' + prefix.as_posix(), '--libdir=' + (prefix / 'lib').as_posix(),
                 '--disable-autodetect', '--disable-everything', '--disable-programs',
                 '--disable-doc', '--disable-debug', '--disable-network', '--disable-avdevice',
                 '--disable-avfilter', '--disable-swscale', '--disable-swresample',
                 '--disable-shared', '--enable-static', '--enable-small', '--enable-zlib',
                 '--disable-x86asm', '--enable-pic', '--enable-parsers',
                 '--extra-version=amule-' + pin['FFMPEG_VERSION'],
                 '--enable-demuxer=' + DEMUXERS]
    if env.get('MSYSTEM', '').startswith('CLANG'):
        configure += ['--cc=clang', '--ar=llvm-ar', '--ranlib=llvm-ranlib']
    elif env.get('CC'):
        configure += ['--cc=' + env['CC']]
    # Existing bundle entry points pass their CMake architecture/deployment values.
    for option in cmake_options:
        if option.startswith('-DCMAKE_OSX_ARCHITECTURES='):
            arch = option.split('=', 1)[1]
            if ';' in arch:
                raise SystemExit('Build one macOS slice at a time, then use the Universal2 lipo job')
            configure += ['--arch=' + arch, '--cc=clang -arch ' + arch]
        elif option.startswith('-DCMAKE_OSX_DEPLOYMENT_TARGET='):
            target = option.split('=', 1)[1]
            configure += ['--extra-cflags=-mmacosx-version-min=' + target,
                          '--extra-ldflags=-mmacosx-version-min=' + target]
        elif option not in ('-G', 'MinGW Makefiles'):
            raise SystemExit('Unsupported FFmpeg build option: ' + option)
    bash = shutil.which('bash', path=env.get('PATH'))
    if not bash:
        raise SystemExit('Bash is required to configure FFmpeg')
    # FFmpeg Makefiles contain MSYS paths. A native make elsewhere on the
    # inherited Windows PATH cannot interpret them, even inside an MSYS shell.
    make = str(Path(bash).with_name('make.exe')) if os.name == 'nt' else shutil.which('make', path=env.get('PATH'))
    if not make or not Path(make).is_file():
        raise SystemExit('MSYS2 make is required on Windows (pacman -S make)')

    def run(command):
        command = list(map(str, command))
        command[0] = {'bash': bash, 'make': make}.get(command[0], command[0])
        print('+', ' '.join(command), flush=True)
        subprocess.run(command, cwd=build, env=env, check=True)
    run(['make', '--version'])
    stamp = build / 'requested-config.json'
    if not stamp.exists() or json.loads(stamp.read_text()) != configure:
        run(['bash', (source / 'configure').as_posix(), *configure])
        stamp.write_text(json.dumps(configure, indent=2) + '\n')
    run(['make', '-j' + str(min(os.cpu_count() or 2, 8))])
    run(['make', 'install'])
    notices = prefix / 'share/licenses/amule-media-probe'
    notices.mkdir(parents=True, exist_ok=True)
    for name in ('COPYING.LGPLv2.1', 'LICENSE.md'):
        shutil.copy2(source / name, notices / ('FFmpeg-' + name))
    shutil.copy2(build / 'config.h', notices / 'ffmpeg-config.h')
    (notices / 'ffmpeg-source.json').write_text(json.dumps(dict(pin, configure=configure), indent=2) + '\n')


if __name__ == '__main__':
    main()
