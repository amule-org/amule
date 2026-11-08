#!/usr/bin/env python3
"""Exercise actual OS denials; no external network or user files are needed."""
from pathlib import Path
import os
import subprocess
import socket
import sys
import tempfile
import unittest
import wave

FIXTURE, HELPER = (str(Path(arg).resolve()) for arg in sys.argv[1:3])
del sys.argv[1:3]


class SandboxTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = Path(self.tmp.name)
        self.media = self.path / 'input.wav'
        with wave.open(str(self.media), 'wb') as media:
            media.setparams((1, 2, 8000, 0, 'NONE', 'not compressed'))
            media.writeframes(bytes(16000))
        self.secret = self.path / 'must-not-read.txt'
        self.secret.write_bytes(b'private fixture, not media')

    def tearDown(self):
        self.tmp.cleanup()

    def run_action(self, action, argument=None, **kwargs):
        with self.media.open('rb') as source:
            result = subprocess.run([FIXTURE, action, str(argument or self.secret)],
                                    stdin=source, capture_output=True, timeout=35, **kwargs)
        self.assertEqual(result.returncode, 0, (action, result.stdout, result.stderr))
        return result

    def test_input_read_and_seek_still_work(self):
        self.run_action('read-input')
        self.run_action('allocate')
        if os.name != 'nt':
            self.assertEqual(self.run_action('vectored').stdout, b'vector ok\n')

    @unittest.skipUnless(os.name == 'nt', 'Windows input handle access reduction')
    def test_writable_input_handle_cannot_modify_the_input(self):
        original = self.media.read_bytes()
        with self.media.open('r+b') as source:
            result = subprocess.run([FIXTURE, 'write-input', str(self.secret)],
                                    stdin=source, capture_output=True, timeout=35)
        self.assertEqual(result.returncode, 0, (result.stdout, result.stderr))
        self.assertEqual(result.stdout, b'blocked\n')
        self.assertEqual(self.media.read_bytes(), original)

    @unittest.skipUnless(os.name == 'nt', 'Windows launcher confinement setup')
    def test_failed_launcher_confinement_returns_session_failure_without_metadata(self):
        with self.media.open('rb') as source:
            result = subprocess.run([FIXTURE, 'launcher-setup-failure', str(self.secret)],
                                    stdin=source, capture_output=True, timeout=35)
        self.assertEqual(result.returncode, 13, result.stderr)
        self.assertEqual(result.stdout, b'')
        self.assertIn(b'create private parser desktop failed', result.stderr)
        self.assertIn(b'Windows error', result.stderr)

    @unittest.skipUnless(os.name == 'nt', 'Windows output pipe containment')
    def test_output_handles_cannot_resize_files_and_output_is_bounded(self):
        self.run_action('resize-output')
        for action in ('overflow-output', 'overflow-errors'):
            with self.media.open('rb') as source:
                result = subprocess.run([FIXTURE, action, str(self.secret)],
                                        stdin=source, capture_output=True, timeout=35)
            self.assertEqual(result.returncode, 12, (action, result.stderr))
            self.assertLessEqual(len(result.stdout), 65536)
            # The trusted launcher's fixed failure diagnostic follows the relay.
            self.assertLessEqual(len(result.stderr), 8192 + 256)
            self.assertIn(b'bounded parser output', result.stderr)

    def test_other_files_cannot_be_read(self):
        self.assertEqual(self.run_action('read-file').stdout, b'blocked\n')

    def test_files_cannot_be_created_or_changed(self):
        self.run_action('write-file')
        self.assertEqual(self.secret.read_bytes(), b'private fixture, not media')
        created = self.path / 'must-not-create.txt'
        self.run_action('write-file', created)
        self.assertFalse(created.exists())

    def test_network_and_new_processes_are_denied(self):
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            listener.listen()
            self.run_action('network', listener.getsockname()[1])
            listener.settimeout(0.05)
            with self.assertRaises(socket.timeout):
                listener.accept()
        self.run_action('network-listen', '0')
        self.run_action('process', FIXTURE)

    @unittest.skipIf(os.name == 'nt', 'POSIX descriptor and exec checks')
    def test_exec_unix_sockets_and_inherited_files_are_denied(self):
        self.run_action('exec', FIXTURE)
        # macOS temporary paths can exceed sockaddr_un's path limit.
        with tempfile.TemporaryDirectory(prefix='ammp-', dir='/tmp') as temporary:
            endpoint = str(Path(temporary) / 'listener')
            with socket.socket(socket.AF_UNIX) as listener:
                listener.bind(endpoint)
                listener.listen()
                self.run_action('unix-network', endpoint)
                listener.settimeout(0.05)
                with self.assertRaises(socket.timeout):
                    listener.accept()
        with self.secret.open('rb') as secret:
            self.run_action('inherited', secret.fileno(), pass_fds=(secret.fileno(),))

    @unittest.skipUnless(sys.platform.startswith('linux'), 'Linux syscall alternative')
    def test_openat_cannot_bypass_the_policy(self):
        self.run_action('openat')

    @unittest.skipUnless(sys.platform.startswith('linux'), 'Linux mapping argument checks')
    def test_file_and_executable_mappings_are_denied(self):
        self.run_action('map-file')
        self.run_action('map-executable')

    def test_nonseekable_input_returns_no_metadata_with_a_reason(self):
        result = subprocess.run([HELPER], input=b'not a regular file', capture_output=True, timeout=35)
        self.assertEqual(result.returncode, 12)
        self.assertEqual(result.stdout, b'')
        self.assertIn(b'input', result.stderr)

    @unittest.skipIf(os.name == 'nt', 'POSIX input access mode check')
    def test_writable_input_is_rejected_before_protocol_header(self):
        with self.media.open('r+b') as source:
            result = subprocess.run([HELPER], stdin=source, capture_output=True, timeout=35)
        self.assertEqual(result.returncode, 13)
        self.assertEqual(result.stdout, b'')
        self.assertIn(b'refusing to parse', result.stderr)


if __name__ == '__main__':
    unittest.main()
