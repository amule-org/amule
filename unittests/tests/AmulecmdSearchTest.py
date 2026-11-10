#!/usr/bin/env python3
#
# This file is part of the aMule Project.
#
# Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
#
# Any parts of this program derived from the xMule, lMule or eMule project,
# or contributed by third-party developers are copyrighted by their
# respective authors.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA
#

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
                op, tags = self.server.reply
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
            'EC_TAG_CAN_SEARCH_LIST', 'EC_TAG_CAN_SEARCH_METADATA')]
    with tempfile.TemporaryDirectory(prefix='amulecmd-search-') as directory:
        with Server(('127.0.0.1', 0), Handler) as server:
            server.capabilities = caps
            server.requests, server.errors = [], []
            server.reply = C['EC_OP_STRINGS'], [integer(C['EC_TAG_SEARCH_ID'], 101)]
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()

            def command(text, expected=None, extra=()):
                server.requests.clear()
                result = subprocess.run([binary, '-h', '127.0.0.1', '-p',
                    str(server.server_address[1]), '-P', 'regression', *extra, '-c', text],
                    env=daemon_env(directory), stdin=subprocess.DEVNULL,
                    capture_output=True, text=True, timeout=20)
                output = result.stdout + result.stderr
                assert not server.errors, server.errors
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
                for option, values in {'--min-bitrate': ['-1', '1.5', '1000001', '18446744073709551616'],
                                       '--min-duration': ['-1', '1:00', '86401', 'x']}.items():
                    for value in values:
                        command(f'search kad alpha {option} {value}', 'Invalid search filter syntax')
                        assert not server.requests
                for suffix in ('--artist', '--title "unfinished', '--album --codec FLAC', '--min-duration'):
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
                server.capabilities = caps
                for action in ('stop', 'more'):
                    for value in ('0', '-1', '1.5', '4294967296', '101 102', 'x'):
                        command(f'search {action} {value}', 'A search ID')
                        assert not server.requests
                server.reply = C['EC_OP_SEARCH_LIST'], [tag(C['EC_TAG_SEARCH_ID'],
                    struct.pack('!I', 101), 4, [string(C['EC_TAG_SEARCH_NAME'], 'query\twith\ncontrols')])]
                command('search list', '101\tquery with controls\n')
                assert server.requests == [(C['EC_OP_SEARCH_LIST'], {})]
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
                print('PASS: media filters, validation, capability guards, listing, stop and more replies')
            finally:
                server.shutdown()
                thread.join(5)


if __name__ == '__main__':
    run(str(Path(sys.argv[1]).resolve()))
