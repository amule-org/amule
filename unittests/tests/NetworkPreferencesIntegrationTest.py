#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Enable ED2K at runtime through REST and synchronize network flags through EC.

Uses an isolated offline daemon, saved server lists and a second EC client, as an
amulegui poll would. No public network connections or existing profiles are used.
"""
import json
from pathlib import Path
import struct
import socket
import sys
import tempfile
import time
import urllib.request

from AllSearchIntegrationTest import (C, configure_amuleapi, free_ports, integer,
                                      retry_port_collisions, running_daemon, start_amuleapi,
                                      start_daemon, stop_daemon, string, tag, wait_amuleapi)
from DaemonConfig import daemon_env, write_amule_conf
from KnownHistoryIntegrationTest import fake_server


def server_met(port):
    return b'\xe0' + struct.pack('<I', 1) + bytes([203, 0, 113, 7]) + struct.pack('<HI', port, 0)


def wait_for(check, message):
    deadline = time.monotonic() + 30
    while not check():
        assert time.monotonic() < deadline, message
        time.sleep(0.1)


def run(daemon_binary, api_binary):
    with tempfile.TemporaryDirectory(prefix='amule-network-prefs-') as temporary:
        root = Path(temporary)
        core, api = root / 'core', root / 'api'
        core.mkdir()
        api.mkdir()
        ec_port, peer_port, udp_port, http_port = free_ports(4, udp_indices=(2,), peer_index=1)
        ports = ec_port, peer_port, udp_port
        write_amule_conf(core, ec_port, Port=peer_port, UDPPort=udp_port, UDPEnable=1,
                         FilterLanIPs=0, Serverlist=1, sections={'WebServer': {'Enabled': 0}})
        (core / 'addresses.dat').write_text('')  # Exercise auto-update without downloading.
        (core / 'server.met').write_bytes(server_met(4661))
        (core / 'staticservers.dat').write_text('203.0.113.8:4662,0,Static regression server\n')
        env = daemon_env(root)
        daemon = server = ec = None
        try:
            daemon, ec = start_daemon(daemon_binary, core, ports, env=env)

            def connection_state():
                op, tags = ec.call(C['EC_OP_STAT_REQ'])
                assert op == C['EC_OP_STATS'], op
                return tags[C['EC_TAG_CONNSTATE']][0]

            def network_flags():
                op, tags = ec.call(C['EC_OP_STAT_REQ'])
                assert op == C['EC_OP_STATS'], op
                return tags[C['EC_TAG_CONNSTATE']][1][C['EC_TAG_NETWORKS_ENABLED']][0]

            assert network_flags() == 0, 'fixture must start with both networks disabled'
            assert not ec.call(C['EC_OP_GET_SERVER_LIST'])[1], 'disabled ED2K loaded servers'
            with (root / 'api.log').open('w') as log:
                configure_amuleapi(api_binary, api, env, log, admin='regression')
                server = start_amuleapi(api_binary, api, ec_port, http_port, env, log)
                opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
                url = f'http://127.0.0.1:{http_port}/api/v1'
                token = None

                def request(path, data=None, method=None):
                    headers = {'Content-Type': 'application/json'}
                    if token:
                        headers['Authorization'] = 'Bearer ' + token
                    req = urllib.request.Request(url + path,
                        data=None if data is None else json.dumps(data).encode(),
                        headers=headers, method=method)
                    with opener.open(req, timeout=5) as response:
                        return json.load(response)

                token = wait_amuleapi(server, url, log.name, password='regression')
                result = request('/preferences', {'connection': {'ed2k_enabled': True}}, 'PATCH')
                assert result['connection']['ed2k_enabled'] is True, result
                assert network_flags() == 1, 'ED2K change missing from EC poll'
                wait_for(lambda: len(request('/servers')['servers']) == 2,
                         'runtime enable did not load server.met and staticservers.dat')
                assert {s['port'] for s in request('/servers')['servers']} == {4661, 4662}
                log_text = (core / 'logfile').read_text(errors='replace')
                assert log_text.count('No server list sources configured') == 1, log_text
                assert f'Created Server UDP-Socket at port {peer_port + 3}' in log_text

                # Change the on-disk list: subsequent toggles must preserve the live list,
                # rather than initializing it again and merging this extra server.
                (core / 'server.met').write_bytes(server_met(4663))
                for ed2k, kad in ((False, True), (True, True), (False, False), (True, False)):
                    request('/preferences', {'connection': {
                        'ed2k_enabled': ed2k, 'kad_enabled': kad}}, 'PATCH')
                    assert network_flags() == int(ed2k) | (int(kad) << 1)
                assert len(request('/servers')['servers']) == 2, 'server list was reinitialized'
                log_text = (core / 'logfile').read_text(errors='replace')
                assert log_text.count('No server list sources configured') == 1

                # Partial EC writes from another client must also feed the poll signal,
                # including a zero bitmask, without changing the other network flag.
                for name, value, expected in (('EC_TAG_NETWORK_KADEMLIA', 1, 3),
                                               ('EC_TAG_NETWORK_ED2K', 0, 2),
                                               ('EC_TAG_NETWORK_KADEMLIA', 0, 0)):
                    fields = tag(C['EC_TAG_PREFS_CONNECTIONS'], children=[integer(C[name], value)])
                    op, _ = ec.call(C['EC_OP_SET_PREFERENCES'],
                                   [integer(C['EC_TAG_DETAIL_LEVEL'], C['EC_DETAIL_FULL']), fields])
                    assert op == C['EC_OP_NOOP'], op
                    assert network_flags() == expected

                # A running Kad with no contacts must stop too, not only a connected Kad.
                request('/preferences', {'connection': {'kad_enabled': True}}, 'PATCH')
                assert ec.call(C['EC_OP_KAD_START'])[0] == C['EC_OP_NOOP']
                wait_for(lambda: connection_state() & 0x10, 'Kad did not start')
                request('/preferences', {'connection': {'kad_enabled': False}}, 'PATCH')
                assert not connection_state() & 0x10, 'disabled Kad is still running'

                # Exercise a real loopback server connection, then disable ED2K from REST.
                request('/preferences', {'connection': {'ed2k_enabled': True}}, 'PATCH')
                port = fake_server()
                assert ec.call(C['EC_OP_SERVER_ADD'], [string(C['EC_TAG_SERVER_ADDRESS'],
                               f'localhost:{port}')])[0] == C['EC_OP_NOOP']
                server_tag = tag(C['EC_TAG_SERVER'], b'\0' * 4 + struct.pack('!H', port), 8)
                assert ec.call(C['EC_OP_SERVER_CONNECT'], [server_tag])[0] == C['EC_OP_NOOP']
                wait_for(lambda: connection_state() & 0x01, 'loopback server did not connect')
                request('/preferences', {'connection': {'ed2k_enabled': False}}, 'PATCH')
                assert not connection_state() & 0x03, 'disabled ED2K is connected or connecting'

                # Hold a second server at the login stage. Disabling ED2K must close
                # an in-flight attempt as well as an established connection.
                with socket.socket() as listener:
                    listener.bind(('127.0.0.1', 0))
                    listener.listen(1)
                    listener.settimeout(30)
                    port = listener.getsockname()[1]
                    request('/preferences', {'connection': {'ed2k_enabled': True}}, 'PATCH')
                    assert ec.call(C['EC_OP_SERVER_ADD'], [string(C['EC_TAG_SERVER_ADDRESS'],
                                   f'localhost:{port}')])[0] == C['EC_OP_NOOP']
                    server_tag = tag(C['EC_TAG_SERVER'], b'\0' * 4 + struct.pack('!H', port), 8)
                    assert ec.call(C['EC_OP_SERVER_CONNECT'], [server_tag])[0] == C['EC_OP_NOOP']
                    with listener.accept()[0] as pending:
                        pending.settimeout(30)
                        assert pending.recv(4096), 'daemon did not send a server login'
                        assert connection_state() & 0x02, 'fixture did not reach a connection attempt'
                        request('/preferences', {'connection': {'ed2k_enabled': False}}, 'PATCH')
                        assert not connection_state() & 0x03, 'disabled ED2K is still connecting'
                        while pending.recv(4096):
                            pass  # EOF proves that the attempt's TCP socket was closed.
        except BaseException:
            for path in (core / 'logfile', core / 'console.log', root / 'api.log'):
                if path.exists():
                    print(path.read_text(errors='replace')[-12000:], file=sys.stderr)
            raise
        finally:
            if server is not None:
                stop_daemon(server, timeout=15, check=sys.exc_info()[0] is None)
            if ec is not None:
                ec.sock.close()
            if daemon is not None:
                # Preserve the original failure rather than masking it in cleanup.
                stop_daemon(daemon, check=sys.exc_info()[0] is None)

        # Disabling ED2K must still save the list that was initialized earlier. The
        # replacement on-disk seed must not win over the live list on shutdown.
        saved = (core / 'server.met').read_bytes()
        assert struct.unpack_from('<I', saved, 1)[0] == 4, 'disabled ED2K lost live server edits'

        # A core started disabled must leave an uninitialized list untouched on exit.
        # Use a fresh minimal record: accidentally loading and saving it would add
        # normalized tags, so the byte comparison detects that regression too.
        unloaded = server_met(4664)
        (core / 'server.met').write_bytes(unloaded)
        with running_daemon(daemon_binary, core, ports, env=env) as (_, ec):
            assert not ec.call(C['EC_OP_GET_SERVER_LIST'])[1], 'disabled restart loaded the list'
        assert (core / 'server.met').read_bytes() == unloaded, 'uninitialized list overwrote saved data'


if __name__ == '__main__':
    retry_port_collisions(run, *(str(Path(arg).resolve()) for arg in sys.argv[1:]))
    print('Runtime network preference checks passed')
