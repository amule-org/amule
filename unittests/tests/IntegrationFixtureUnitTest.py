#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline regression checks for integration fixture port selection and diagnostics."""
import configparser
from pathlib import Path
import errno
import hashlib
import os
import signal
import struct
import subprocess
import sys
from itertools import cycle
import tempfile
import time
import unittest
from unittest.mock import patch

import AllSearchIntegrationTest
import DaemonConfig
import LogfilePathIntegrationTest
from OfferFilesInteropPeer import login_tags


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


class StopDaemonUnitTest(unittest.TestCase):
    def spawn(self, code):
        return subprocess.Popen([sys.executable, '-c', code])

    def test_unclean_exit_fails_check(self):
        if os.name == 'nt':
            self.skipTest('taskkill exit codes are not checked on Windows')
        proc = self.spawn('import time; time.sleep(60)')
        with patch.object(AllSearchIntegrationTest.subprocess.Popen, 'terminate',
                          lambda p: p.send_signal(signal.SIGINT)):
            with self.assertRaises(AssertionError):
                AllSearchIntegrationTest.stop_daemon(proc)
        self.assertIsNotNone(proc.poll())

    def test_codes_accepts_an_expected_exit(self):
        if os.name == 'nt':
            self.skipTest('POSIX signals only')
        code = ('import signal, sys, time\n'
                'signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))\ntime.sleep(60)')
        proc = self.spawn(code)
        time.sleep(0.5)
        AllSearchIntegrationTest.stop_daemon(proc, codes=(0, 1))
        proc = self.spawn(code)
        time.sleep(0.5)
        with self.assertRaises(AssertionError):
            AllSearchIntegrationTest.stop_daemon(proc)

    def test_hung_process_is_killed(self):
        if os.name == 'nt':
            self.skipTest('POSIX signals only')
        proc = self.spawn('import signal, time\n'
                          'signal.signal(signal.SIGTERM, signal.SIG_IGN)\ntime.sleep(60)')
        time.sleep(0.5)
        with self.assertRaises(AssertionError):
            AllSearchIntegrationTest.stop_daemon(proc, timeout=1)
        self.assertIsNotNone(proc.poll())
        proc = self.spawn('import signal, time\n'
                          'signal.signal(signal.SIGTERM, signal.SIG_IGN)\ntime.sleep(60)')
        time.sleep(0.5)
        AllSearchIntegrationTest.stop_daemon(proc, timeout=1, check=False)
        self.assertIsNotNone(proc.poll())

    def test_exited_process_is_not_signalled(self):
        proc = self.spawn('pass')
        proc.wait()
        with patch.object(AllSearchIntegrationTest.subprocess.Popen, 'terminate') as terminate:
            AllSearchIntegrationTest.stop_daemon(proc)
        terminate.assert_not_called()


class OfferFilesLoginUnitTest(unittest.TestCase):
    def test_offerfiles_login_request_with_other_extensions(self):
        standard = (b'\x02\x01\x00\x01\x04\x00test'
                    + b''.join(b'\x03\x01\x00' + bytes([name]) + struct.pack('<I', 1)
                               for name in (0x11, 0x20, 0xfb)))
        request = b'\x03\x0c\x00offerfiles_v\x01\x00\x00\x00'
        ipv6 = b'\x01\x01\x00\xae' + bytes.fromhex('20014860000000000000000000008888')
        for extensions in (request, request + ipv6, ipv6 + request):
            count = 4 + 1 + (ipv6 in extensions)
            payload = bytes(22) + struct.pack('<I', count) + standard + extensions
            self.assertEqual(login_tags(payload)[b'offerfiles_v'], 1)
        payload = bytes(22) + struct.pack('<I', 5) + standard + ipv6
        self.assertNotIn(b'offerfiles_v', login_tags(payload))
        # The request bytes inside a nickname are not an actual extension.
        nickname = b'\x02\x01\x00\x01' + struct.pack('<H', len(request)) + request
        payload = bytes(22) + struct.pack('<I', 4) + nickname + standard[10:]
        self.assertNotIn(b'offerfiles_v', login_tags(payload))

    def test_offerfiles_login_rejects_wrong_request_integer_width(self):
        for kind, width in ((8, 2), (9, 1), (11, 8)):
            request = bytes([kind]) + b'\x0c\x00offerfiles_v' + (1).to_bytes(width, 'little')
            payload = bytes(22) + struct.pack('<I', 1) + request
            with self.assertRaisesRegex(AssertionError, 'must use uint32'):
                login_tags(payload)

    def test_offerfiles_login_rejects_truncated_hash_tags(self):
        payload = bytes(22) + struct.pack('<I', 1) + b'\x01\x01\x00\xae' + bytes(15)
        with self.assertRaisesRegex(AssertionError, 'truncated tag value'):
            login_tags(payload)


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

    def test_daemon_env_isolates_home_and_drops_preload(self):
        with patch.dict(DaemonConfig.os.environ, {'LD_PRELOAD': 'x.so', 'DISPLAY': ':0'}):
            env = DaemonConfig.daemon_env('/tmp/home', AMULE_TEST_X=1, DISPLAY=None)
            preload = DaemonConfig.daemon_env('/tmp/home', LD_PRELOAD='y.so')
        self.assertEqual(env['HOME'], '/tmp/home')
        self.assertEqual(env['XDG_CONFIG_HOME'], str(Path('/tmp/home') / 'xdg'))
        self.assertEqual(env['LC_ALL'], 'C.UTF-8')
        self.assertEqual(env['AMULE_TEST_X'], '1')
        self.assertNotIn('LD_PRELOAD', env)
        self.assertNotIn('DISPLAY', env)
        self.assertEqual(preload['LD_PRELOAD'], 'y.so')

    def test_none_leaves_a_key_out(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            DaemonConfig.write_amule_conf(root, 4712, TempDir=None, IncomingDir=None)
            text = (root / 'amule.conf').read_text(encoding='utf-8')
        self.assertNotIn('TempDir=', text)
        self.assertNotIn('IncomingDir=', text)
        self.assertIn('GeoIPEnabled=0', text)

    def test_remote_conf_targets_the_core_in_english(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            DaemonConfig.write_remote_conf(root, 4712)
            config = configparser.ConfigParser()
            config.optionxform = str
            config.read(root / 'remote.conf', encoding='utf-8')
        self.assertEqual(config['EC']['Host'], '127.0.0.1')
        self.assertEqual(config['EC']['Port'], '4712')
        self.assertEqual(config['EC']['Password'],
                         hashlib.md5(DaemonConfig.EC_PASSWORD.encode()).hexdigest())
        self.assertEqual(config['eMule']['Language'], DaemonConfig.OFFLINE['Language'])

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
