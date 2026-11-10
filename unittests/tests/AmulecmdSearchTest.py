#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the text client's search commands through authenticated EC connections."""
from pathlib import Path
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading
import zlib

from ECWire import accept, tag as capability
from AllSearchIntegrationTest import C, exact, integer, parse_tags, string, tag
from DaemonConfig import daemon_env


def expand_numbers(body):
    """Expand compact EC tag headers so the maintained tag decoder can read them."""
    offset = 0

    def number():
        nonlocal offset
        first = body[offset]
        offset += 1
        if first < 128:
            return first
        count = 0
        mask = 128
        while first & mask:
            count += 1
            mask >>= 1
        assert 2 <= count <= 6
        value = first & ((1 << (7 - count)) - 1)
        for _ in range(count - 1):
            part = body[offset]
            offset += 1
            assert part & 192 == 128
            value = (value << 6) | (part & 63)
        return value

    def one_tag():
        nonlocal offset
        name, kind, length = number(), number(), number()
        children = b''
        count_bytes = b''
        if name & 1:
            count = number()
            count_bytes = struct.pack('!H', count)
            children = b''.join(one_tag() for _ in range(count))
        value_length = length - len(children)
        assert value_length >= 0
        value = body[offset:offset + value_length]
        offset += value_length
        return struct.pack('!HBI', name, kind, length) + count_bytes + children + value

    op, count = number(), number()
    expanded = bytes([op]) + struct.pack('!H', count) + b''.join(one_tag() for _ in range(count))
    assert offset == len(body), (offset, len(body))
    return expanded


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        try:
            self.request.settimeout(15)
            self.request = accept(self.request, capabilities=self.server.capabilities)
            while True:
                flags, size = struct.unpack('!II', exact(self.request, 8))
                body = exact(self.request, size)
                if flags & 1:
                    body = zlib.decompress(body)
                if flags & 2:
                    body = expand_numbers(body)
                fields, _ = parse_tags(body, 3, struct.unpack_from('!H', body, 1)[0])
                self.server.requests.append((body[0], fields))
                op, tags = (self.server.reply(body[0], fields) if callable(self.server.reply)
                            else self.server.reply)
                payload = bytes([op]) + struct.pack('!H', len(tags)) + b''.join(tags)
                self.request.sendall(struct.pack('!II', 0x20, len(payload)) + payload)
        except (EOFError, ConnectionError):
            pass
        except BaseException as error:
            self.server.errors.append(error)


class Server(socketserver.ThreadingTCPServer):
    daemon_threads = True


def run(binary):
    caps = [capability(name) for name in ('EC_TAG_CAN_MULTI_SEARCH',
            'EC_TAG_CAN_SEARCH_LIST', 'EC_TAG_CAN_SEARCH_METADATA', 'EC_TAG_CAN_SEARCH_ALL')]
    with tempfile.TemporaryDirectory(prefix='amulecmd-search-') as directory:
        with Server(('127.0.0.1', 0), Handler) as server:
            server.capabilities = caps
            server.requests, server.errors = [], []
            server.reply = C['EC_OP_STRINGS'], [integer(C['EC_TAG_SEARCH_ID'], 101)]
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()

            def command(text, expected=None, extra=(), interactive=False):
                server.requests.clear()
                result = subprocess.run([binary, '-h', '127.0.0.1', '-p',
                    str(server.server_address[1]), '-P', 'regression', *extra, *([] if interactive else ['-c', text])],
                    env=daemon_env(directory), input=text if interactive else '',
                    capture_output=True, text=True, timeout=20)
                output = result.stdout + result.stderr
                assert not server.errors, server.errors
                assert result.returncode == 0, (text, result.returncode, output)
                if expected:
                    assert expected in output, (text, output)
                return output

            try:
                command('search kad alpha OR beta --min-bitrate 1000000 --min-duration 86400 '
                        '--codec FLAC --artist "Test Artist" --album \'Test Album\' '
                        '--title "曲 AND OR NOT" --type Audio --ext mp3 --avail 7 '
                        '--min-size 10M --max-size 8G', 'Search started (id 101)')
                op, fields = server.requests[0]
                assert op == C['EC_OP_SEARCH_START']
                kind, filters = fields[C['EC_TAG_SEARCH_TYPE']]
                assert kind == C['EC_SEARCH_KAD']
                for name, expected in {'MIN_BITRATE': 1000000, 'MIN_LENGTH': 86400,
                        'MIN_SIZE': 10 * 1024**2, 'MAX_SIZE': 8 * 1024**3,
                        'AVAILABILITY': 7, 'NAME': b'alpha OR beta\0', 'CODEC': b'FLAC\0',
                        'ARTIST': b'Test Artist\0', 'ALBUM': b'Test Album\0',
                        'TITLE': '曲 AND OR NOT\0'.encode(), 'FILE_TYPE': b'Audio\0',
                        'EXTENSION': b'mp3\0'}.items():
                    assert filters[C['EC_TAG_SEARCH_' + name]][0] == expected, (name, filters)
                # Both entry modes pass through the shared command dispatcher. Quoted
                # media values must survive it byte-for-byte, and quoted query words
                # must never turn into options.
                for text, expected_query, expected_artist in (
                        ('search kad alpha --artist "  Test\t  Artist  "', b'alpha\0', b'  Test\t  Artist  \0'),
                        ('search kad "alpha --artist literal" --artist Real', b'"alpha --artist literal"\0', b'Real\0'),
                        ('search kad ("alpha --title literal") OR beta --artist Real',
                         b'("alpha --title literal") OR beta\0', b'Real\0')):
                    command(text, 'Search started')
                    fields = server.requests[0][1][C['EC_TAG_SEARCH_TYPE']][1]
                    assert fields[C['EC_TAG_SEARCH_NAME']][0] == expected_query, fields
                    assert fields[C['EC_TAG_SEARCH_ARTIST']][0] == expected_artist, fields
                    assert C['EC_TAG_SEARCH_TITLE'] not in fields, fields
                for kind in ('local', 'global', 'kad', 'all'):
                    command(f'search {kind} --artist First alpha --artist "Last Artist" --min-duration 1',
                            'Search started')
                    search_kind, fields = server.requests[0][1][C['EC_TAG_SEARCH_TYPE']]
                    assert search_kind == C['EC_SEARCH_' + kind.upper()]
                    assert fields[C['EC_TAG_SEARCH_ARTIST']][0] == b'Last Artist\0'
                    assert fields[C['EC_TAG_SEARCH_MIN_LENGTH']][0] == 1
                    assert fields[C['EC_TAG_SEARCH_NAME']][0] == b'alpha\0'
                command('search kad alpha --artist "" --codec "" --title "" --album ""', 'Search started')
                fields = server.requests[0][1][C['EC_TAG_SEARCH_TYPE']][1]
                assert not any(C['EC_TAG_SEARCH_' + name] in fields for name in ('ARTIST', 'CODEC', 'TITLE', 'ALBUM'))
                for option, values in {'--min-bitrate': ['-1', '1.5', '1000001', '18446744073709551616'],
                                       '--min-duration': ['-1', '1:00', '86401', 'x']}.items():
                    for value in values:
                        command(f'search kad alpha {option} {value}', 'Invalid search filter syntax')
                        assert not server.requests
                for suffix in ('--artist', '--title "unfinished', '--album --codec FLAC', '--min-duration',
                               '--title "closed"suffix'):
                    command('search kad alpha ' + suffix, 'Invalid search filter syntax')
                    assert not server.requests
                server.capabilities = []
                for suffix in ('--min-bitrate 128', '--min-duration 60', '--codec FLAC',
                               '--title Title', '--album Album', '--artist Artist'):
                    command('search kad alpha ' + suffix, 'does not support media search filters')
                    assert not server.requests
                command('search kad "alpha beta" --min-bitrate 0 --min-duration 0', 'Search started')
                filters = server.requests[0][1][C['EC_TAG_SEARCH_TYPE']][1]
                assert filters[C['EC_TAG_SEARCH_NAME']][0] == b'"alpha beta"\0'
                assert C['EC_TAG_SEARCH_MIN_BITRATE'] not in filters
                for text in ('search list', 'search stop 101', 'search more 101'):
                    command(text, 'does not support search management')
                    assert not server.requests
                for partial in ([capability('EC_TAG_CAN_MULTI_SEARCH')], [capability('EC_TAG_CAN_SEARCH_LIST')]):
                    server.capabilities = partial
                    for text in ('search list', 'search stop 101', 'search more 101'):
                        command(text, 'does not support search management')
                        assert not server.requests
                server.capabilities = caps
                for action in ('stop', 'more'):
                    for value in ('0', '-1', '1.5', '4294967296', '101 102', 'x'):
                        command(f'search {action} {value}', 'A search ID')
                        assert not server.requests
                server.reply = C['EC_OP_SEARCH_LIST'], [tag(C['EC_TAG_SEARCH_ID'],
                    struct.pack('!I', 101), 4, [string(C['EC_TAG_SEARCH_NAME'], 'query\twith\ncontrols')])]
                command('search list', '101\tquery with controls\n')
                assert server.requests == [(C['EC_OP_SEARCH_LIST'], {})]
                command('search list', '101 query\twith controls\n', extra=('--space-separated',))
                server.reply = C['EC_OP_SEARCH_LIST'], []
                command('search list', 'No retained searches')
                server.reply = C['EC_OP_MISC_DATA'], []
                command('search stop 4294967295', 'retained results were not removed')
                assert server.requests == [(C['EC_OP_SEARCH_STOP'],
                    {C['EC_TAG_SEARCH_ID']: (4294967295, {})})]
                for verdict, expected in ((None, 'availability is unknown'),
                        (1, 'further requests remain possible'), (0, 'No further results')):
                    server.reply = C['EC_OP_MISC_DATA'], ([] if verdict is None else
                        [integer(C['EC_TAG_SEARCH_MORE_REASKABLE'], verdict)])
                    command('search more 101', expected)
                    assert server.requests == [(C['EC_OP_SEARCH_REQUEST_MORE'],
                        {C['EC_TAG_SEARCH_ID']: (101, {})})]
                server.reply = C['EC_OP_FAILED'], [string(C['EC_TAG_STRING'], 'fixture rejection')]
                command('search more 101', 'fixture rejection')
                def reply_for(op, fields):
                    if op == C['EC_OP_SEARCH_START']:
                        return C['EC_OP_STRINGS'], [integer(C['EC_TAG_SEARCH_ID'], 101)]
                    if op == C['EC_OP_SEARCH_LIST']:
                        return C['EC_OP_SEARCH_LIST'], [tag(C['EC_TAG_SEARCH_ID'], struct.pack('!I', 101), 4,
                            [string(C['EC_TAG_SEARCH_NAME'], 'session query')])]
                    if op == C['EC_OP_SEARCH_REQUEST_MORE']:
                        return C['EC_OP_MISC_DATA'], [integer(C['EC_TAG_SEARCH_MORE_REASKABLE'], 0)]
                    if op == C['EC_OP_SEARCH_STOP']:
                        return C['EC_OP_MISC_DATA'], []
                    if op == C['EC_OP_SEARCH_RESULTS']:
                        return C['EC_OP_SEARCH_RESULTS'], []
                    if op == C['EC_OP_SEARCH_PROGRESS']:
                        return C['EC_OP_SEARCH_PROGRESS'], [integer(C['EC_TAG_SEARCH_STATUS'], 50)]
                    return C['EC_OP_NOOP'], []

                server.reply = reply_for
                out = command('search kad invalid --min-duration -1\n  SeArCh  KaD alpha --artist "  Test  Artist  "\n'
                    'search kad cleanquery\nsearch list\nsearch more 101\nsearch stop 101\nprogress 101\nresults 101\n'
                    'set   bwlimit  up   32\nquit\n', '101\tsession query', interactive=True)
                assert 'No further results' in out and 'retained results were not removed' in out, out
                assert [op for op, _ in server.requests] == [C[name] for name in (
                    'EC_OP_SEARCH_START', 'EC_OP_SEARCH_START', 'EC_OP_SEARCH_LIST', 'EC_OP_SEARCH_REQUEST_MORE',
                    'EC_OP_SEARCH_STOP', 'EC_OP_SEARCH_PROGRESS', 'EC_OP_SEARCH_RESULTS',
                    'EC_OP_SET_PREFERENCES', 'EC_OP_GET_PREFERENCES')], server.requests
                fields = server.requests[0][1][C['EC_TAG_SEARCH_TYPE']][1]
                assert fields[C['EC_TAG_SEARCH_ARTIST']][0] == b'  Test  Artist  \0', fields
                fields = server.requests[1][1][C['EC_TAG_SEARCH_TYPE']][1]
                assert fields[C['EC_TAG_SEARCH_NAME']][0] == b'cleanquery\0'
                assert C['EC_TAG_SEARCH_ARTIST'] not in fields, fields
                assert 'Invalid search filter syntax' in out, out
                for _, fields in server.requests[3:7]:
                    assert fields[C['EC_TAG_SEARCH_ID']][0] == 101, fields
                print('PASS: media filters, validation, capability guards, listing, stop and more replies')
            finally:
                server.shutdown()
                thread.join(5)


if __name__ == '__main__':
    run(str(Path(sys.argv[1]).resolve()))
