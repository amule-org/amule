#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise Release logging configuration, EC verbose toggles and marker compatibility."""
import configparser
from pathlib import Path
import socket
import struct
import sys
import tempfile

from AllSearchIntegrationTest import C, free_ports, integer, retry_port_collisions, string, tag
from DaemonConfig import write_amule_conf
from PathDefaultsIntegrationTest import daemon
from PacketReceiveIntegrationTest import expect_hello_answer, send_hello


def verbose(ec, enabled):
    assert ec.call(C['EC_OP_SET_PREFERENCES'], [
        integer(C['EC_TAG_DETAIL_LEVEL'], C['EC_DETAIL_FULL']),
        tag(C['EC_TAG_PREFS_CORETWEAKS'], children=[
            integer(C['EC_TAG_CORETW_VERBOSE'], enabled)])
    ])[0] == C['EC_OP_NOOP']


def emit(ec, root, message, expected, logfile_only):
    assert ec.call(C['EC_OP_ADDDEBUGLOGLINE'], [
        string(C['EC_TAG_STRING'], message)])[0] == C['EC_OP_NOOP']
    # The whole-log EC read is a barrier and returns the raw file, including markers.
    op, tags = ec.call(C['EC_OP_GET_LOG'])
    assert op == C['EC_OP_LOG']
    ec_log = tags[C['EC_TAG_STRING']][0].decode()
    lines = (root / 'logfile').read_text().splitlines()
    matching = [line for line in lines if message in line]
    assert bool(matching) == expected, (message, matching)
    if expected:
        assert matching[0][0] == ('.' if logfile_only else ' '), matching
    assert (message in ec_log) == expected, ec_log


def emit_peer_details(ec, root, port, expected, logfile_only):
    with socket.create_connection(('127.0.0.1', port), timeout=10) as peer:
        frame = send_hello(peer)
        peer_port = peer.getsockname()[1]
        payload = struct.pack('<BBI', 0x30, 0x01, 1) + struct.pack('<BHBI', 3, 1, 0xfe, 123)
        peer.sendall(struct.pack('<BIB', 0xc5, len(payload) + 1, 0x02) + payload)
        peer.sendall(frame)
        expect_hello_answer(peer)
        ec.call(C['EC_OP_GET_LOG'])
        lines = (root / 'logfile').read_text().splitlines()
        matching = [line for line in lines if 'Unknown Mule tag' in line
                    and f'IP:Port 127.0.0.1:{peer_port} ' in line]
        assert bool(matching) == expected, (expected, lines)
        if expected:
            assert matching[0][0] == ('.' if logfile_only else ' '), matching


def run(binary):
    with tempfile.TemporaryDirectory(prefix='amule-runtime-logging-') as temp:
        for logfile_only in (False, True):
            root = Path(temp) / str(logfile_only)
            root.mkdir()
            ports = free_ports(2, peer_index=1)
            write_amule_conf(root, ports[0], Port=ports[1], UDPEnable=0,
                             FilterLanIPs=0,
                             VerboseDebug=0, VerboseDebugLogfile=int(logfile_only),
                             sections={'Debug': {'Cat_General': 1, 'Cat_Hasher': 0,
                                                 'Cat_Packet Parsing Errors': 1}})
            with daemon(binary, root, ports) as ec:
                emit(ec, root, 'runtime-logging-disabled', False, logfile_only)
                emit_peer_details(ec, root, ports[1], False, logfile_only)
                verbose(ec, True)
                emit(ec, root, 'runtime-logging-enabled', True, logfile_only)
                emit_peer_details(ec, root, ports[1], True, logfile_only)
                verbose(ec, False)
                emit(ec, root, 'runtime-logging-disabled-again', False, logfile_only)
                emit_peer_details(ec, root, ports[1], False, logfile_only)
                verbose(ec, True)
            config = configparser.ConfigParser(interpolation=None)
            config.read(root / 'amule.conf')
            assert config['eMule'].getboolean('VerboseDebug')
            assert config['eMule'].getboolean('VerboseDebugLogfile') == logfile_only
            assert config['Debug'].getboolean('Cat_General')
            assert not config['Debug'].getboolean('Cat_Hasher')
            with daemon(binary, root, ports) as ec:
                emit(ec, root, 'runtime-logging-after-restart', True, logfile_only)
                emit_peer_details(ec, root, ports[1], True, logfile_only)
    print('Runtime logging integration checks passed')


if __name__ == '__main__':
    retry_port_collisions(run, str(Path(sys.argv[1]).resolve()))
