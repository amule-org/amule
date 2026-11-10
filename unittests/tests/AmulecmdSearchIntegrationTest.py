#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""The real command client records media filters and stops only its named daemon search."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from AllSearchIntegrationTest import (C, integer, retry_port_collisions, stop_daemon,
                                      stored_result, stored_search)
from DaemonConfig import daemon_env
from SearchParamsRestoreIntegrationTest import boot, search_list


def run(daemon, client):
    with tempfile.TemporaryDirectory(prefix='amulecmd-search-daemon-') as directory:
        root = Path(directory)
        (root / 'StoredSearches.met').write_bytes(stored_search([stored_result('retained.bin')]))
        (proc, ec), ports = boot(daemon, root)

        def command(text, expected):
            result = subprocess.run([client, '-h', '127.0.0.1', '-p', str(ports[0]),
                '-P', 'regression', '-c', text], env=daemon_env(directory),
                stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=20)
            output = result.stdout + result.stderr
            assert expected in output, (text, result.returncode, output)
            return output

        try:
            command('search list', '123\t')
            before = ec.call_list(C['EC_OP_SEARCH_RESULTS'], [integer(C['EC_TAG_SEARCH_ID'], 123)])
            assert before[1], before
            command('search stop 123', 'retained results were not removed')
            assert ec.call_list(C['EC_OP_SEARCH_RESULTS'], [integer(C['EC_TAG_SEARCH_ID'], 123)]) == before
            assert ec.call(C['EC_OP_KAD_START'])[0] == C['EC_OP_NOOP']
            output = command('search kad cmdmetadata --min-bitrate 320 --min-duration 3723 '
                '--codec FLAC --artist "Test Artist" --album "Test Album" --title "曲 Title"',
                'Search started (id ')
            sid = int(re.search(r'Search started \(id (\d+)\)', output)[1])
            fields = search_list(ec)[sid]
            for name, expected in {'MIN_BITRATE': 320, 'MIN_LENGTH': 3723}.items():
                assert int.from_bytes(fields[C['EC_TAG_SEARCH_' + name]], 'big') == expected
            for name, expected in {'CODEC': 'FLAC', 'ARTIST': 'Test Artist',
                                   'ALBUM': 'Test Album', 'TITLE': '曲 Title'}.items():
                assert fields[C['EC_TAG_SEARCH_' + name]].rstrip(b'\0').decode() == expected
            other = ec.start('unaffectedquery', kind=C['EC_SEARCH_KAD'])
            assert ec.progress(sid)[C['EC_TAG_SEARCH_KAD_ACTIVE']][0] == 1
            command(f'search more {sid}', 'More-results request processed')
            command(f'search stop {sid}', 'retained results were not removed')
            assert ec.progress(sid)[C['EC_TAG_SEARCH_KAD_ACTIVE']][0] == 0
            assert ec.progress(other)[C['EC_TAG_SEARCH_KAD_ACTIVE']][0] == 1
            assert sid in search_list(ec)
            command(f'search more {sid}', 'No further results')
            command(f'search stop {sid}', 'retained results were not removed')
            command('search stop 4294967295', 'retained results were not removed')
            command('search more 4294967295', 'No further results')
            assert ec.progress(other)[C['EC_TAG_SEARCH_KAD_ACTIVE']][0] == 1
            command('search kad invalid --min-bitrate 1000001', 'Invalid search filter syntax')
            assert set(search_list(ec)) == {123, sid, other}

            command('search stop 123', 'retained results were not removed')
            assert ec.call_list(C['EC_OP_SEARCH_RESULTS'], [integer(C['EC_TAG_SEARCH_ID'], 123)]) == before
            print('PASS: real daemon records all media filters; stop retains results and leaves other searches active')
        except BaseException:
            for name in ('console.log', 'logfile'):
                path = root / name
                if path.exists():
                    print(path.read_text(errors='replace'), file=sys.stderr)
            raise
        finally:
            ec.sock.close()
            stop_daemon(proc, check=sys.exc_info()[0] is None)

        # Restart the same configuration: the command client's recorded form and the
        # pre-existing retained result must survive, not only its immediate EC reply.
        (proc, ec), ports = boot(daemon, root)
        try:
            command('search list', str(sid) + '\tcmdmetadata')
            fields = search_list(ec)[sid]
            assert int.from_bytes(fields[C['EC_TAG_SEARCH_MIN_LENGTH']], 'big') == 3723
            assert int.from_bytes(fields[C['EC_TAG_SEARCH_MIN_BITRATE']], 'big') == 320
            for name, expected in {'CODEC': 'FLAC', 'ARTIST': 'Test Artist',
                                   'ALBUM': 'Test Album', 'TITLE': '曲 Title'}.items():
                assert fields[C['EC_TAG_SEARCH_' + name]].rstrip(b'\0').decode() == expected
            results = ec.call_list(C['EC_OP_SEARCH_RESULTS'], [integer(C['EC_TAG_SEARCH_ID'], 123)])
            assert results[1], results
        finally:
            ec.sock.close()
            stop_daemon(proc, check=sys.exc_info()[0] is None)


if __name__ == '__main__':
    retry_port_collisions(run, *(str(Path(arg).resolve()) for arg in sys.argv[1:]))
