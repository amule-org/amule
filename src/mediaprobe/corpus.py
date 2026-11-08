#!/usr/bin/env python3
"""Generate, validate and reproducibly archive the media parser's fuzz corpus.

Only generated test media and fuzzer mutations belong here; never personal media.
Archives contain regular files named by SHA-256, with bounded input/total sizes.
"""
import argparse
import gzip
import hashlib
import io
import os
from pathlib import Path
import subprocess
import sys
import tarfile

MAX_INPUT = 262144
MAX_TOTAL = 128 * 1024 * 1024
MAX_FILES = 10000
# Include tar headers, per-file padding and the final record. Bound decompression
# before tarfile interprets GNU/PAX headers, which can themselves request memory.
MAX_TAR_BYTES = MAX_TOTAL + MAX_FILES * 1024 + 10240


def save(data):
    destination = os.environ.get('MEDIA_PROBE_SAVE_CORPUS')
    if destination and len(data) <= MAX_INPUT:
        directory = Path(destination)
        directory.mkdir(parents=True, exist_ok=True)
        (directory / hashlib.sha256(data).hexdigest()).write_bytes(data)


def pack(directory, archive):
    entries = {}
    total = 0
    for path in sorted(directory.iterdir()):
        if path.is_symlink() or not path.is_file():
            raise ValueError(f'not a regular corpus input: {path.name}')
        if not 0 <= path.stat().st_size <= MAX_INPUT:
            raise ValueError(f'input size out of range: {path.name}')
        data = path.read_bytes()
        name = hashlib.sha256(data).hexdigest()
        if name not in entries:
            total += len(data)
            entries[name] = data
        if total > MAX_TOTAL or len(entries) > MAX_FILES:
            raise ValueError('corpus exceeds archive limits')
    if not entries:
        raise ValueError('refusing to archive an empty corpus')
    # Stable ordering, ownership and timestamps make corpus updates reviewable.
    with archive.open('wb') as output:
        with gzip.GzipFile(filename='', mode='wb', fileobj=output, mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode='w') as tar:
                for name, data in sorted(entries.items()):
                    info = tarfile.TarInfo(name)
                    info.size, info.mode = len(data), 0o644
                    tar.addfile(info, io.BytesIO(data))
    print(f'Archived {len(entries)} inputs, {total} bytes; SHA256 '
          f'{hashlib.sha256(archive.read_bytes()).hexdigest()}')


def unpack(archive, directory):
    total = 0
    entries = {}
    if archive.stat().st_size > MAX_TAR_BYTES * 2:
        raise ValueError('compressed corpus archive exceeds size limit')
    with gzip.open(archive, 'rb') as compressed:
        contents = compressed.read(MAX_TAR_BYTES + 1)
    if len(contents) > MAX_TAR_BYTES:
        raise ValueError('decompressed corpus archive exceeds size limit')
    with tarfile.open(fileobj=io.BytesIO(contents), mode='r:') as tar:
        for member in tar:
            if (not member.isfile() or len(member.name) != 64 or
                    any(c not in '0123456789abcdef' for c in member.name) or
                    not 0 <= member.size <= MAX_INPUT or member.name in entries):
                raise ValueError('invalid corpus archive entry')
            total += member.size
            if total > MAX_TOTAL or len(entries) >= MAX_FILES:
                raise ValueError('corpus exceeds archive limits')
            data = tar.extractfile(member).read()
            if hashlib.sha256(data).hexdigest() != member.name:
                raise ValueError('corpus input checksum mismatch')
            entries[member.name] = data
    if not entries:
        raise ValueError('empty corpus archive')
    # Validate the entire archive before writing anything. Never use extractall.
    directory.mkdir(parents=True, exist_ok=True)
    for name, data in entries.items():
        target = directory / name
        # Existing directories/symlinks must not redirect writes outside the corpus.
        with target.open('xb') as output:
            output.write(data)
    print(f'Loaded {len(entries)} inputs, {total} bytes')


def seed(helper, directory):
    directory.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ, MEDIA_PROBE_SAVE_CORPUS=str(directory.resolve()))
    for script in ('test_helper.py', 'test_hostile.py'):
        subprocess.run([sys.executable, str(Path(__file__).with_name(script)), str(helper.resolve())],
                       env=environment, check=True, timeout=180)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    for command in ('pack', 'unpack'):
        child = commands.add_parser(command)
        child.add_argument('directory' if command == 'pack' else 'archive', type=Path)
        child.add_argument('archive' if command == 'pack' else 'directory', type=Path)
    child = commands.add_parser('seed')
    child.add_argument('helper', type=Path)
    child.add_argument('directory', type=Path)
    args = parser.parse_args()
    if args.command == 'seed':
        seed(args.helper, args.directory)
    elif args.command == 'pack':
        pack(args.directory, args.archive)
    else:
        unpack(args.archive, args.directory)


if __name__ == '__main__':
    main()
