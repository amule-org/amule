#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare all six published fields on a pinned corpus or a real media directory.

Examples:
  compare.py --corpus CORPUS --helper bundled=PATH --helper system=PATH --output report.json
  compare.py --directory MEDIA --helper bundled=PATH --helper system=PATH --output real.json
Media stays outside the repository; report filenames only with permission.
"""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import subprocess

FIELDS = ('length_seconds', 'bitrate_kbps', 'codec', 'artist', 'album', 'title')
TAGS = dict(artist='Spike Artist', album='Spike Album', title='Spike Title')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--corpus', type=Path)
    source.add_argument('--directory', type=Path)
    parser.add_argument('--helper', action='append', required=True, help='LABEL=EXECUTABLE')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    helpers = dict(item.split('=', 1) for item in args.helper)
    helpers = {key: str(Path(value).resolve()) for key, value in helpers.items()}
    root = args.corpus or args.directory
    if args.corpus:
        cases = json.loads((root / 'manifest.json').read_text())['cases']
    else:
        cases = [dict(file=p.name, sha256=hashlib.sha256(p.read_bytes()).hexdigest())
                 for p in sorted(root.iterdir()) if p.suffix.lower() in
                 ('.mp4', '.mkv', '.webm', '.avi', '.mov', '.ogv', '.ogg', '.mp3', '.m4a', '.flac', '.wav', '.flv',
                  '.rm', '.rmvb', '.ra', '.mpc', '.mxf', '.wtv', '.au', '.caf', '.dv', '.ivf', '.m4v', '.w64', '.dts', '.m1v', '.m3u8', '.ffconcat', '.sdp', '.mpd')]
    report = dict(platform=platform.platform(), versions={}, cases=[], summary={})
    for key, command in helpers.items():
        report['versions'][key] = subprocess.check_output([command, '--version'], text=True).strip()
    for case in cases:
        path = root / case['file']
        checksum = hashlib.sha256(path.read_bytes()).hexdigest()
        if checksum != case['sha256']:
            raise SystemExit('Corpus checksum mismatch: ' + case['file'])
        row = dict(file=case['file'], sha256=checksum, bytes=path.stat().st_size, results={})
        for name, command in helpers.items():
            with path.open('rb') as media:
                result = subprocess.run([command, '--json'], stdin=media, capture_output=True, timeout=30)
            metadata = json.loads(result.stdout) if result.returncode == 0 else {}
            row['results'][name] = dict(exit=result.returncode,
                metadata={key: metadata.get(key) for key in FIELDS})
        row['differences'] = {key: {name: value['metadata'][key] for name, value in row['results'].items()}
                              for key in FIELDS if len({value['metadata'][key] for value in row['results'].values()}) > 1}
        exits = {name: result['exit'] for name, result in row['results'].items()}
        if len(set(exits.values())) > 1:
            row['differences']['exit'] = exits
        report['cases'].append(row)
    for name in helpers:
        values = [row['results'][name] for row in report['cases']]
        score = dict(extracted=sum(row['exit'] == 0 for row in values), files=len(cases))
        if args.corpus:
            score.update(codec_correct=sum(row['metadata']['codec'] == case['expected_codec']
                         for row, case in zip(values, cases)),
                duration_correct=sum(row['metadata']['length_seconds'] == case['expected_seconds']
                         for row, case in zip(values, cases) if case['expected_seconds'] is not None),
                duration_cases=sum(case['expected_seconds'] is not None for case in cases),
                requested_tags=sum(row['metadata'][key] == value for row in values for key, value in TAGS.items()))
        report['summary'][name] = score
    report['identical_files'] = sum(not row['differences'] for row in report['cases'])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(dict(summary=report['summary'], identical_files=report['identical_files']), indent=2))


if __name__ == '__main__':
    main()
