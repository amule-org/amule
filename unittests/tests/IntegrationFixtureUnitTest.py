#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline regression checks for integration fixture port selection and diagnostics."""
import configparser
from pathlib import Path
import errno
import hashlib
from itertools import cycle
import tempfile
import unittest
from unittest.mock import patch

import AllSearchIntegrationTest
import DaemonConfig
import LogfilePathIntegrationTest


class ReusingSocket:
    """Model an allocator that reuses a port immediately after socket close."""
    sockets = []

    def __init__(self):
        self.port = None
        self.closed = False
        self.sockets.append(self)

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.closed = True

    def bind(self, address):
        assert address == ('127.0.0.1', 0)
        occupied = {sock.port for sock in self.sockets if not sock.closed}
        self.port = next(port for port in range(40000, 40004) if port not in occupied)

    def getsockname(self):
        return '127.0.0.1', self.port


class PortConflictSocket:
    """Force a peer+3 collision and an occupied client UDP port."""
    tcp_ports = iter(())
    sockets = []
    udp_binds = []

    def __init__(self, *args):
        self.udp = len(args) > 1 and args[1] == AllSearchIntegrationTest.socket.SOCK_DGRAM
        self.port = None
        self.closed = False
        self.sockets.append(self)

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.closed = True

    def bind(self, address):
        assert address[0] == '127.0.0.1'
        if self.udp:
            self.udp_binds.append(address[1])
            if address[1] == 40012:
                raise OSError(errno.EADDRINUSE, 'UDP port occupied')
            self.port = address[1]
        else:
            assert address[1] == 0
            self.port = next(self.tcp_ports)

    def getsockname(self):
        return '127.0.0.1', self.port


class FailedDaemon:
    def __init__(self, *args, stdout, **kwargs):
        stdout.buffer.write(b'\xff ERROR: cannot open log file\n')
        stdout.flush()

    def wait(self, **kwargs):
        return 1

    def poll(self):
        return 1


class IntegrationFixtureUnitTest(unittest.TestCase):
    def test_ports_remain_reserved_until_all_are_chosen(self):
        ReusingSocket.sockets = []
        with patch.object(AllSearchIntegrationTest.socket, 'socket', ReusingSocket):
            ports = AllSearchIntegrationTest.free_ports(4)
        self.assertEqual(ports, (40000, 40001, 40002, 40003))
        self.assertTrue(all(sock.closed for sock in ReusingSocket.sockets))

    def test_udp_conflicts_are_retried(self):
        PortConflictSocket.tcp_ports = iter((40000, 40001, 40004,
                                             40010, 40011, 40012,
                                             40020, 40021, 40022))
        PortConflictSocket.sockets = []
        PortConflictSocket.udp_binds = []
        with patch.object(AllSearchIntegrationTest.socket, 'socket', PortConflictSocket):
            ports = AllSearchIntegrationTest.free_ports(3, udp_indices=(2,), peer_index=1)
        self.assertEqual(ports, (40020, 40021, 40022))
        self.assertEqual(PortConflictSocket.udp_binds, [40012, 40022, 40024])
        self.assertTrue(all(sock.closed for sock in PortConflictSocket.sockets))

    def test_persistent_udp_conflict_fails_without_hanging(self):
        PortConflictSocket.tcp_ports = cycle((40010, 40011, 40012))
        PortConflictSocket.sockets = []
        PortConflictSocket.udp_binds = []
        with patch.object(AllSearchIntegrationTest.socket, 'socket', PortConflictSocket):
            with self.assertRaisesRegex(AllSearchIntegrationTest.PortCollision,
                                        'Could not select available fixture UDP ports'):
                AllSearchIntegrationTest.free_ports(3, udp_indices=(2,), peer_index=1)
        self.assertEqual(PortConflictSocket.udp_binds, [40012] * 32)
        self.assertTrue(all(sock.closed for sock in PortConflictSocket.sockets))

    def test_startup_retry_after_late_udp_collision(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            attempts = []

            def start():
                attempts.append(len(attempts))
                log = ('*** TCP socket (ECServer) listening on 127.0.0.1:40000\n'
                       'ListenSocket: Ok.\n')
                if len(attempts) == 2:
                    log += 'Created Client UDP-Socket at port 40002\n'
                (root / 'logfile').write_text(log)
                AllSearchIntegrationTest.check_daemon_ports(root, 40000, 40001, 40002)

            AllSearchIntegrationTest.retry_port_collisions(start)
            self.assertEqual(len(attempts), 2)

    def test_unrelated_error_is_not_retried(self):
        attempts = []

        def start():
            attempts.append(None)
            raise ValueError('unrelated startup failure')

        with self.assertRaisesRegex(ValueError, 'unrelated startup failure'):
            AllSearchIntegrationTest.retry_port_collisions(start)
        self.assertEqual(len(attempts), 1)

    def test_ec_does_not_connect_to_a_stolen_port(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'logfile').write_text(
                'Could not listen for external connections at 127.0.0.1:40000!\n')
            proc = type('RunningDaemon', (), {'poll': lambda self: None})()
            with patch.object(AllSearchIntegrationTest, 'EC') as ec:
                with self.assertRaises(AllSearchIntegrationTest.PortCollision):
                    AllSearchIntegrationTest.connect_daemon(proc, 40000, root, 40001)
                ec.assert_not_called()

    def test_startup_log_ignores_previous_launch(self):
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / 'logfile'
            log.write_bytes(b'previous success\n')
            snapshot = AllSearchIntegrationTest.log_snapshot(log)
            self.assertEqual(AllSearchIntegrationTest.new_log_text(log, snapshot), '')
            with log.open('ab') as stream:
                stream.write(b'current failure\n')
            self.assertEqual(AllSearchIntegrationTest.new_log_text(log, snapshot),
                             'current failure\n')
            log.write_bytes(b'truncated and regrown with different startup lines\n')
            self.assertEqual(AllSearchIntegrationTest.new_log_text(log, snapshot),
                             'truncated and regrown with different startup lines\n')
            replacement = Path(temporary) / 'replacement'
            replacement.write_bytes(b'new file success\n')
            replacement.replace(log)
            self.assertEqual(AllSearchIntegrationTest.new_log_text(log, snapshot),
                             'new file success\n')

    def test_invalid_console_bytes_preserve_startup_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / 'fixture'
            with patch.object(LogfilePathIntegrationTest.subprocess, 'Popen', FailedDaemon):
                LogfilePathIntegrationTest.run('unused-daemon', root, failure=True,
                                               expected=root / 'logfile')


class DaemonConfigUnitTest(unittest.TestCase):
    def test_offline_defaults_overrides_and_sections(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            DaemonConfig.write_amule_conf(root, 4712, Port=4662, ConnectToKad=1,
                                          sections={'WebServer': {'Enabled': 0}})
            config = configparser.ConfigParser(interpolation=None)
            config.optionxform = str
            config.read(root / 'amule.conf', encoding='utf-8')
        emule = config['eMule']
        for key, value in DaemonConfig.OFFLINE.items():
            if key != 'ConnectToKad':
                self.assertEqual(emule[key], str(value), key)
        self.assertEqual(emule['GeoIPEnabled'], '0')
        self.assertEqual(emule['NewVersionCheck'], '0')
        self.assertEqual(emule['ConnectToKad'], '1')
        self.assertEqual(emule['Port'], '4662')
        self.assertEqual(emule['TempDir'], str(root / 'Temp'))
        self.assertEqual(emule['IncomingDir'], str(root / 'Incoming'))
        self.assertEqual(config['ExternalConnect']['ECPort'], '4712')
        self.assertEqual(config['ExternalConnect']['ECPassword'],
                         hashlib.md5(DaemonConfig.EC_PASSWORD.encode()).hexdigest())
        self.assertEqual(config['WebServer']['Enabled'], '0')

    def test_none_leaves_a_key_out(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            DaemonConfig.write_amule_conf(root, 4712, TempDir=None, IncomingDir=None)
            text = (root / 'amule.conf').read_text(encoding='utf-8')
        self.assertNotIn('TempDir=', text)
        self.assertNotIn('IncomingDir=', text)
        self.assertIn('GeoIPEnabled=0', text)

    def test_no_test_writes_its_own_daemon_config(self):
        # A hand-written eMule section misses later offline defaults; use write_amule_conf.
        marker = '[' + 'eMule]'
        unittests = Path(__file__).resolve().parent.parent
        writers = [path.relative_to(unittests).as_posix()
                   for path in sorted(unittests.rglob('*.py'))
                   if marker in path.read_text(encoding='utf-8', errors='replace')]
        self.assertEqual(writers, [])


if __name__ == '__main__':
    unittest.main()
