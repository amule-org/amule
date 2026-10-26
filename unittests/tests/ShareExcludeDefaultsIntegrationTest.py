#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the shipped exclusion default and preservation of saved preferences."""
import configparser
from pathlib import Path
import sys
import tempfile

from AllSearchIntegrationTest import C, retry_port_collisions
from CompletedFileDeleteIntegrationTest import eventually
from PathDefaultsIntegrationTest import daemon, read_config, write_config
from ShareWatcherMoveIntegrationTest import shared


def run(binary):
    with tempfile.TemporaryDirectory(prefix='amule-share-defaults-') as scratch:
        root = Path(scratch) / 'profile'
        ports = write_config(root, {'ExcludeSharePatterns': None})
        incoming = root / 'Incoming'
        incoming.mkdir()
        names = ('letter.DOCX', 'keep.txt', 'Thumbs.db', 'custom.secret')
        for name in names:
            (incoming / name).write_bytes(name.encode())

        def check(patterns, regex, expected):
            with daemon(binary, root, ports) as ec:
                assert ec.call(C['EC_OP_SET_PREFERENCES'])[0] == C['EC_OP_NOOP']
                saved = read_config(root)
                if patterns is None:
                    assert '*.docx' in saved['ExcludeSharePatterns'].split('|')
                    assert 'Thumbs.db' in saved['ExcludeSharePatterns'].split('|')
                else:
                    assert saved['ExcludeSharePatterns'] == patterns
                assert saved['ExcludeSharePatternsUseRegex'] == str(regex)
                # The expected positive set also makes failures to preserve an empty/custom
                # list observable through the daemon's actual share scan.
                eventually(lambda: set(shared(ec)) == expected)

        default_expected = {'keep.txt', 'custom.secret'}
        check(None, 0, default_expected)
        check(None, 0, default_expected)
        # Existing profile with its key removed receives the default as well.
        for patterns, regex, expected in (
                (None, 0, default_expected),
                ('Thumbs.db', 0, set(names) - {'Thumbs.db'}),
                ('*.secret', 0, set(names) - {'custom.secret'}),
                ('', 0, set(names)),
                (r'\.secret$', 1, set(names) - {'custom.secret'})):
            config = configparser.ConfigParser(interpolation=None)
            config.optionxform = str
            config.read(root / 'amule.conf', encoding='utf-8')
            if patterns is None:
                config.remove_option('eMule', 'ExcludeSharePatterns')
            else:
                config['eMule']['ExcludeSharePatterns'] = patterns
            config['eMule']['ExcludeSharePatternsUseRegex'] = str(regex)
            with (root / 'amule.conf').open('w', encoding='utf-8') as out:
                config.write(out, space_around_delimiters=False)
            check(patterns, regex, expected)
            check(patterns, regex, expected)
    print('Share exclusion defaults, saved lists, empty opt-out and regex survive restart')


if __name__ == '__main__':
    retry_port_collisions(run, str(Path(sys.argv[1]).resolve()))
