#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Search start parameters survive a StoredSearches.met round-trip and older files still load.

The start parameters ride in a trailing block after the 0x01 records, keyed by search id, so a
build that predates the block restores the searches and drops only the filters. The writer must
emit exactly the filters a search started with (and omit the defaulted ones), the reader must
accept every current tag plus unknown future ones, a file without the block or a block with an
unknown marker must restore the searches without parameters, and a damaged block (a known tag of
the wrong type, or more forms than searches) must restore them without parameters too.
"""
from pathlib import Path
import struct
import sys
import tempfile

from AllSearchIntegrationTest import (C, exact, free_ports, retry_port_collisions,
                                      stored_int_tag, stored_param_tags, stored_search,
                                      stored_string_tag, stop_daemon, string, tag)
from DaemonConfig import write_amule_conf
from KnownHistoryIntegrationTest import start
from ShareWatcherMoveIntegrationTest import tag_list

FULL = {'query': 'comprehensive filters alpha', 'file_type': 'Audio', 'extension': '.mp3',
        'avail': 7, 'min_size': 1 << 20, 'max_size': (1 << 33) + 5,
        'min_bitrate': 320, 'min_length': 3723, 'codec': 'FLAC',
        'title': '曲 AND OR NOT', 'album': 'Test Album', 'artist': 'Test Artist'}
SPARSE = {'query': 'simple types beta', 'file_type': 'Video'}


def uint64(name, value):
    return tag(name, struct.pack('!Q', value), 5)


def search_request(query, kind=C['EC_SEARCH_KAD'], file_type='', extension='', avail=0,
                   min_size=0, max_size=0, min_bitrate=0, min_length=0,
                   codec="", title="", album="", artist=""):
    """The EC_OP_SEARCH_START compound tag, as CEC_Search_Tag builds it."""
    children = [string(C['EC_TAG_SEARCH_NAME'], query),
                string(C['EC_TAG_SEARCH_FILE_TYPE'], file_type)]
    if extension:
        children.append(string(C['EC_TAG_SEARCH_EXTENSION'], extension))
    if avail:
        children.append(tag(C['EC_TAG_SEARCH_AVAILABILITY'], struct.pack('!I', avail), 4))
    if min_size:
        children.append(uint64(C['EC_TAG_SEARCH_MIN_SIZE'], min_size))
    if max_size:
        children.append(uint64(C['EC_TAG_SEARCH_MAX_SIZE'], max_size))
    for name, value in [('MIN_BITRATE', min_bitrate), ('MIN_LENGTH', min_length)]:
        if value:
            children.append(uint64(C['EC_TAG_SEARCH_' + name], value))
    for name, value in [('CODEC', codec), ('TITLE', title), ('ALBUM', album), ('ARTIST', artist)]:
        if value:
            children.append(string(C['EC_TAG_SEARCH_' + name], value))
    return tag(C['EC_TAG_SEARCH_TYPE'], bytes([kind]), 2, children)


def start_search(ec, query, **params):
    op, tags = ec.call(C['EC_OP_SEARCH_START'], [search_request(query, **params)])
    assert op == C['EC_OP_STRINGS'], (op, tags)
    assert C['EC_TAG_SEARCH_KAD_ACTIVE'] in tags, tags
    return tags[C['EC_TAG_SEARCH_ID']][0]


def parse_stored(data):
    """Version and per-search parameters of a StoredSearches.met with no results."""
    version = data[0]
    count = struct.unpack_from('<I', data, 1)[0]
    offset = 5
    searches = []
    for _ in range(count):
        sid = struct.unpack_from('<I', data, offset)[0]
        offset += 4
        length = struct.unpack_from('<H', data, offset)[0]
        offset += 2
        query = data[offset:offset + length]
        offset += length
        kind = data[offset]
        offset += 1 + 8  # kind byte + start time
        params = {}
        results = struct.unpack_from('<I', data, offset)[0]
        offset += 4
        assert results == 0, 'fixture parser only handles searches without results'
        searches.append({'id': sid, 'query': query, 'kind': kind, 'params': params})
    # Trailing start-parameter block, keyed by search id.
    if offset < len(data):
        assert struct.unpack_from('<I', data, offset)[0] == 0x534D5052, \
            'fixture parser: unknown trailing marker'
        offset += 4
        entries = struct.unpack_from('<I', data, offset)[0]
        offset += 4
        for _ in range(entries):
            sid = struct.unpack_from('<I', data, offset)[0]
            offset += 4
            for entry in searches:
                if entry['id'] == sid:
                    offset = read_param_tags(data, offset, entry['params'])
                    break
            else:
                raise AssertionError(f'fixture parser: params for unknown search id {sid}')
    return version, searches


def read_param_tags(data, offset, params):
    """The named tag list of one search's start parameters; returns the new offset."""
    tags = data[offset]
    offset += 1
    for _ in range(tags):
        ttype = data[offset]
        offset += 1
        length = struct.unpack_from('<H', data, offset)[0]
        offset += 2
        name = data[offset:offset + length].decode()
        offset += length
        if ttype == 0x02:  # TAGTYPE_STRING
            length = struct.unpack_from('<H', data, offset)[0]
            offset += 2
            value = data[offset:offset + length].decode()
            offset += length
        elif ttype == 0x09:  # TAGTYPE_UINT8
            value = data[offset]
            offset += 1
        elif ttype == 0x08:  # TAGTYPE_UINT16
            value = struct.unpack_from('<H', data, offset)[0]
            offset += 2
        elif ttype == 0x03:  # TAGTYPE_UINT32
            value = struct.unpack_from('<I', data, offset)[0]
            offset += 4
        elif ttype == 0x0b:  # TAGTYPE_UINT64
            value = struct.unpack_from('<Q', data, offset)[0]
            offset += 8
        else:
            raise AssertionError(f'fixture parser: unknown tag type 0x{ttype:02x}')
        params[name] = value
    return offset


def search_list(ec):
    """Every EC_OP_SEARCH_LIST entry as {search id: {tag name: raw value}}."""
    payload = bytes([C['EC_OP_SEARCH_LIST']]) + struct.pack('!H', 0)
    ec.sock.sendall(struct.pack('!II', 0x20, len(payload)) + payload)
    _, length = struct.unpack('!II', exact(ec.sock, 8))
    reply = exact(ec.sock, length)
    entries = {}
    for name, value, children in tag_list(reply, 3, struct.unpack_from('!H', reply, 1)[0])[0]:
        if name == C['EC_TAG_SEARCH_ID']:
            entries[int.from_bytes(value, 'big')] = {child: raw for child, raw, _ in children}
    return entries


def s(raw):
    return raw.rstrip(b'\0').decode()


def as_int(raw):
    return int.from_bytes(raw, 'big')


def boot(binary, root):
    for directory in ('Incoming', 'Temp'):
        (root / directory).mkdir(exist_ok=True)
    ports = free_ports(3, udp_indices=(2,), peer_index=1)
    write_amule_conf(root, ports[0], Port=ports[1], UDPPort=ports[2], ConnectToKad=1,
                     ConnectToED2K=1, FilterLanIPs=0)
    (root / 'nodes.dat').write_bytes(struct.pack('<III', 0, 1, 0))
    return start(binary, root, ports), ports


def round_trip(binary, root):
    """A search started with every filter must reload into the same form."""
    (proc, ec), _ = boot(binary, root)
    try:
        assert ec.call(C['EC_OP_KAD_START'])[0] == C['EC_OP_NOOP']
        full = start_search(ec, **FULL)
        # Out-of-range integers must fail without starting or replacing a search.
        before = set(search_list(ec))
        bad_children = [uint64(C['EC_TAG_SEARCH_MIN_BITRATE'], 1 << 32),
                        uint64(C['EC_TAG_SEARCH_MIN_LENGTH'], 86401)]
        for child in bad_children:
            request = tag(C['EC_TAG_SEARCH_TYPE'], bytes([C['EC_SEARCH_KAD']]), 2,
                          [string(C['EC_TAG_SEARCH_NAME'], 'invalidmetadata'),
                           string(C['EC_TAG_SEARCH_FILE_TYPE'], ''), child])
            op, reply = ec.call(C['EC_OP_SEARCH_START'],
                                [request, tag(C['EC_TAG_SEARCH_REF'], struct.pack('!I', 4242), 4)])
            assert op == C['EC_OP_FAILED']
            assert reply[C['EC_TAG_SEARCH_REF']][0] == 4242
            assert set(search_list(ec)) == before
        sparse = start_search(ec, **SPARSE)
    finally:
        ec.sock.close()
        stop_daemon(proc)

    version, searches = parse_stored((root / 'StoredSearches.met').read_bytes())
    assert version == 1, version
    by_id = {entry['id']: entry for entry in searches}
    assert by_id[full]['params'] == {'search.file_type': 'Audio', 'search.extension': '.mp3',
                                     'search.min_size_bytes': FULL['min_size'],
                                     'search.max_size_bytes': FULL['max_size'],
                                     'search.min_source_count': FULL['avail'],
                                     'search.min_bitrate_kbps': FULL['min_bitrate'],
                                     'search.min_length': FULL['min_length'],
                                     **{'search.' + k: FULL[k] for k in ('codec', 'title', 'album', 'artist')}}
    # Defaulted filters are omitted, not written as zeros.
    assert by_id[sparse]['params'] == {'search.file_type': 'Video'}, by_id[sparse]

    (proc, ec), _ = boot(binary, root)
    try:
        listing = search_list(ec)
        assert set(listing) == {full, sparse}, (full, sparse, set(listing))
        fields = listing[full]
        assert s(fields[C['EC_TAG_SEARCH_FILE_TYPE']]) == 'Audio'
        assert s(fields[C['EC_TAG_SEARCH_EXTENSION']]) == '.mp3'
        assert as_int(fields[C['EC_TAG_SEARCH_MIN_SIZE']]) == FULL['min_size']
        assert as_int(fields[C['EC_TAG_SEARCH_MAX_SIZE']]) == FULL['max_size']
        assert as_int(fields[C['EC_TAG_SEARCH_AVAILABILITY']]) == FULL['avail']
        assert as_int(fields[C['EC_TAG_SEARCH_MIN_BITRATE']]) == FULL['min_bitrate']
        assert as_int(fields[C['EC_TAG_SEARCH_MIN_LENGTH']]) == FULL['min_length']
        for name in ('codec', 'title', 'album', 'artist'):
            assert s(fields[C['EC_TAG_SEARCH_' + name.upper()]]) == FULL[name]
        fields = listing[sparse]
        assert s(fields[C['EC_TAG_SEARCH_FILE_TYPE']]) == 'Video'
        assert C['EC_TAG_SEARCH_EXTENSION'] not in fields
        assert as_int(fields[C['EC_TAG_SEARCH_MIN_SIZE']]) == 0
        assert as_int(fields[C['EC_TAG_SEARCH_MAX_SIZE']]) == 0
        assert as_int(fields[C['EC_TAG_SEARCH_AVAILABILITY']]) == 0
    finally:
        ec.sock.close()
        stop_daemon(proc)


def every_parameter_fixture():
    """A file carrying every parameter the format knows, plus one from the future."""
    tags = stored_param_tags(
        stored_string_tag('search.file_type', 'Iso'),
        stored_string_tag('search.extension', '.iso'),
        stored_int_tag('search.min_size_bytes', 1 << 20),
        stored_int_tag('search.max_size_bytes', (1 << 40) + 9),
        stored_int_tag('search.min_source_count', 9),
        stored_string_tag('search.future_filter', 'written by a newer build'),
    )
    return stored_search([], kind=C['EC_SEARCH_KAD'], params=tags)


def current_version_file(binary, root):
    """A file written by the current version must load, including unknown future filters."""
    (root / 'StoredSearches.met').write_bytes(every_parameter_fixture())
    (proc, ec), _ = boot(binary, root)
    try:
        fields = search_list(ec)[123]
        assert s(fields[C['EC_TAG_SEARCH_FILE_TYPE']]) == 'Iso'
        assert s(fields[C['EC_TAG_SEARCH_EXTENSION']]) == '.iso'
        assert as_int(fields[C['EC_TAG_SEARCH_MIN_SIZE']]) == 1 << 20
        assert as_int(fields[C['EC_TAG_SEARCH_MAX_SIZE']]) == (1 << 40) + 9
        assert as_int(fields[C['EC_TAG_SEARCH_AVAILABILITY']]) == 9
    finally:
        ec.sock.close()
        stop_daemon(proc)
    # A clean shutdown re-saves: every known tag survives with its value; the unknown
    # future tag is dropped because this build does not know how to write it.
    version, searches = parse_stored((root / 'StoredSearches.met').read_bytes())
    assert version == 1
    assert searches[0]['params'] == {
        'search.file_type': 'Iso', 'search.extension': '.iso',
        'search.min_size_bytes': 1 << 20, 'search.max_size_bytes': (1 << 40) + 9,
        'search.min_source_count': 9,
    }, searches[0]


def assert_no_form(fields):
    for name in ('FILE_TYPE', 'EXTENSION', 'MIN_SIZE', 'MAX_SIZE', 'AVAILABILITY'):
        assert C['EC_TAG_SEARCH_' + name] not in fields, (name, fields)


def wrong_type_fixture():
    """A known parameter name carrying the wrong tag type is corruption."""
    name = b'search.min_size_bytes'
    value = b'not an integer'
    bad = b'\x02' + struct.pack('<H', len(name)) + name + struct.pack('<H', len(value)) + value
    return stored_search([], params=stored_param_tags(bad))


def over_count_fixture():
    """A block claiming more forms than the file holds searches."""
    params = stored_param_tags(stored_string_tag('search.file_type', 'Iso'))
    return stored_search([]) + struct.pack('<III', 0x534D5052, 2, 123) + params


def damaged_block_keeps_searches(fixture, reason):
    """A damaged block costs only the forms: the search and its results restore without one."""
    def phase(binary, root):
        (root / 'StoredSearches.met').write_bytes(fixture())
        (proc, ec), _ = boot(binary, root)
        try:
            assert_no_form(search_list(ec)[123])
            log = (root / 'logfile').read_text(errors='replace')
            assert 'Invalid entry in stored search list' in log and reason in log, log[-2000:]
        finally:
            ec.sock.close()
            stop_daemon(proc)
    return phase


def legacy_file(binary, root):
    """A 0x01 file restores the search but carries no parameters at all."""
    (root / 'StoredSearches.met').write_bytes(stored_search([]))
    (proc, ec), _ = boot(binary, root)
    try:
        fields = search_list(ec)[123]
        assert_no_form(fields)
    finally:
        ec.sock.close()
        stop_daemon(proc)


def unknown_marker_file(binary, root):
    """Trailing data with an unknown marker restores the searches without forms."""
    (root / 'StoredSearches.met').write_bytes(stored_search([]) + struct.pack('<I', 0xDEADBEEF))
    (proc, ec), _ = boot(binary, root)
    try:
        fields = search_list(ec)[123]
        assert_no_form(fields)
        log = (root / 'logfile').read_text(errors='replace')
        assert 'corrupted, contains invalid header' not in log, log[-2000:]
    finally:
        ec.sock.close()
        stop_daemon(proc)


def main():
    binary = str(Path(sys.argv[1]).resolve())

    def attempt():
        with tempfile.TemporaryDirectory(prefix='amule-search-params-') as scratch:
            base = Path(scratch)
            for name, phase in (('roundtrip', round_trip), ('current', current_version_file),
                                ('wrongtype', damaged_block_keeps_searches(
                                    wrong_type_fixture, 'is not an integer')),
                                ('overcount', damaged_block_keeps_searches(
                                    over_count_fixture, 'holds 2 forms for 1 searches')),
                                ('legacy', legacy_file), ('unknownmarker', unknown_marker_file)):
                root = base / name
                root.mkdir()
                phase(binary, root)
    retry_port_collisions(attempt)
    print('PASS: search start parameters round-trip, current-version restore, '
          'damaged-block restore, legacy restore, and unknown-marker restore')


if __name__ == '__main__':
    main()
