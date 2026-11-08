#!/usr/bin/env python3
#
# This file is part of the aMule Project.
#
# Copyright (c) 2026 aMule Team ( https://amule-org.github.io )
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
#

"""Verify received names, hashes and sizes using the maintained EC peer."""
import configparser
import json
import os
from pathlib import Path
import sys

root = Path(__file__).resolve().parent
sys.path.insert(0, str(root.parents[3] / 'unittests/tests'))
from AllSearchIntegrationTest import C, EC

state = Path(os.environ['ED2K_TEST_STATE'])
config = configparser.ConfigParser()
config.read(state / 'amule/amule.conf')
ec = EC(config.getint('ExternalConnect', 'ECPort'))
try:
    opcode, rows = ec.call_list(C['EC_OP_GET_DLOAD_QUEUE'])
    assert opcode == C['EC_OP_DLOAD_QUEUE'], opcode
    actual = {}
    for name, _, children in rows:
        if name == C['EC_TAG_PARTFILE']:
            fields = {key: value for key, value, _ in children}
            actual[fields[C['EC_TAG_PARTFILE_HASH']].hex()] = {
                'name': fields[C['EC_TAG_PARTFILE_NAME']].rstrip(b'\0').decode(),
                'size': int.from_bytes(fields[C['EC_TAG_PARTFILE_SIZE_FULL']], 'big'),
            }
    expected = json.loads((state / 'expected-transfers.json').read_text())
    by_hash = {row['hash']: {'name': row['name'], 'size': row['size']} for row in expected}
    assert len(by_hash) == len(expected), 'Repeated fixture identity'
    assert actual == by_hash, {'actual': actual, 'expected': by_hash}
    print(f"Verified {len(actual)} queue entries: exact hash, filename and size; no extra entries")
finally:
    ec.sock.close()
