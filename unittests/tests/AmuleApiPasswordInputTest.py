#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline CLI regression tests with disposable profiles and native terminal input."""
import configparser
import hashlib
import json
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
import time
import unittest

BINARY = os.path.abspath(sys.argv.pop(1))


class PasswordInputTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='amuleapi-password-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def command(self, *args):
        return [BINARY, '--config-dir=' + str(self.root), *args]

    def run_cli(self, *args, data=b'secret\n'):
        return subprocess.run(self.command(*args), input=data, capture_output=True, timeout=10)

    def assert_ok(self, result):
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)

    def credentials(self):
        return (self.root / 'amuleapi-passwords').read_bytes()

    def test_admin_and_guest_stdin(self):
        self.assert_ok(self.run_cli('--set-admin-pass-stdin', data=b'admin secret\n'))
        admin = self.credentials().splitlines()[0]
        self.assertNotIn(b'admin secret', self.credentials())
        self.assert_ok(self.run_cli('--set-guest-pass-stdin', data=b'guest secret\r\n'))
        self.assertEqual(self.credentials().splitlines()[0], admin)
        self.assert_ok(self.run_cli('--set-guest-pass-stdin', data=b'-\n'))
        self.assertNotIn(b'guest=\n', self.credentials())
        self.assert_ok(self.run_cli('--set-guest-pass-stdin', data=b'\n'))
        self.assertIn(b'guest=\n', self.credentials())
        self.assertEqual(self.credentials().splitlines()[0], admin)

    def test_eof_does_not_clear_guest(self):
        self.assert_ok(self.run_cli('--set-guest-pass-stdin', data=b'keep-me\n'))
        before = self.credentials()
        result = self.run_cli('--set-guest-pass-stdin', data=b'')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'EOF', result.stderr)
        self.assertEqual(self.credentials(), before)

    def test_input_failure_has_no_filesystem_side_effects(self):
        for data in (b'', b'\x00\n', b'\xff\n', b'\n'):
            result = self.run_cli('--set-admin-pass-stdin', data=data)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(list(self.root.iterdir()), [])

    def test_read_error_has_no_filesystem_side_effects(self):
        if os.name != 'posix':
            self.skipTest('directory descriptor input is POSIX only')
        descriptor = os.open(self.root, os.O_RDONLY)
        try:
            result = subprocess.run(self.command('--set-admin-pass-stdin'), stdin=descriptor,
                                    capture_output=True, timeout=10)
        finally:
            os.close(descriptor)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(list(self.root.iterdir()), [])

    def test_unterminated_line_and_no_confirmation_for_pipe(self):
        self.assert_ok(self.run_cli('--set-admin-pass-stdin', data='密碼 space '.encode()))
        self.assert_ok(self.run_cli('--set-ec-pass-stdin', data=b'secret with spaces '))
        cfg = configparser.ConfigParser()
        cfg.read(self.root / 'amuleapi.conf')
        self.assertEqual(cfg['EC']['PasswordMD5'], hashlib.md5(b'secret with spaces ').hexdigest())

    def test_ec_setup_migrates_plaintext(self):
        conf = self.root / 'amuleapi.conf'
        conf.write_text('[EC]\nPassword=old-secret\nHost=example.invalid\nPort=1234\n'
                        '[Server]\nPort=9876\n[Custom]\nKeep=yes\n')
        conf.chmod(0o600)
        token = self.root / 'amuleapi-ec-token'
        token.write_text('must-not-be-consumed\n')
        self.assert_ok(self.run_cli('--set-ec-pass-stdin', data=b'new-secret\nignored\n'))
        cfg = configparser.ConfigParser()
        cfg.read(conf)
        self.assertEqual(cfg['EC']['PasswordMD5'], hashlib.md5(b'new-secret').hexdigest())
        self.assertNotIn('Password', cfg['EC'])
        self.assertEqual(cfg['EC']['Host'], 'example.invalid')
        self.assertEqual(cfg['Server']['Port'], '9876')
        self.assertEqual(cfg['Custom']['Keep'], 'yes')
        self.assertEqual(token.read_text(), 'must-not-be-consumed\n')
        self.assertFalse((self.root / 'amuleapi.log').exists())
        if os.name == 'posix':
            self.assertEqual(conf.stat().st_mode & 0o777, 0o600)

    def test_empty_ec_input_preserves_existing_config(self):
        self.assert_ok(self.run_cli('--set-ec-pass-stdin'))
        before = (self.root / 'amuleapi.conf').read_bytes()
        self.assertNotEqual(self.run_cli('--set-ec-pass-stdin', data=b'\n').returncode, 0)
        self.assertEqual((self.root / 'amuleapi.conf').read_bytes(), before)

    def test_conflicts_are_explicit_and_fail(self):
        for args in [('--set-admin-pass-stdin', '--set-guest-pass-stdin'),
                     ('--set-admin-pass-stdin', '--set-ec-pass-stdin'),
                     ('--set-guest-pass-stdin', '--set-ec-pass-stdin')]:
            result = self.run_cli(*args)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b'exactly one', result.stderr)
            self.assertEqual(list(self.root.iterdir()), [])

    def test_argv_passwords_are_refused(self):
        for option in ('--set-admin-pass=literal', '--set-guest-pass=literal'):
            result = self.run_cli(option)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(list(self.root.iterdir()), [])

    def test_setup_repairs_bad_ec_fields_and_role_setup_ignores_them(self):
        conf = self.root / 'amuleapi.conf'
        token = self.root / 'amuleapi-ec-token'
        for content in ('[EC]\nPasswordMD5=invalid\n',
                        '[EC]\nPassword=plain\nPasswordMD5=' + 'a' * 32 + '\n'):
            conf.write_text(content)
            conf.chmod(0o600)
            token.write_text('must-not-be-consumed\n')
            for setter in ('--set-admin-pass-stdin', '--set-guest-pass-stdin'):
                self.assert_ok(self.run_cli(setter))
                self.assertEqual(conf.read_text(), content)
            self.assert_ok(self.run_cli('--set-ec-pass-stdin'))
            cfg = configparser.ConfigParser()
            cfg.read(conf)
            self.assertEqual(cfg['EC']['PasswordMD5'], hashlib.md5(b'secret').hexdigest())
            self.assertNotIn('Password', cfg['EC'])
            self.assertEqual(token.read_text(), 'must-not-be-consumed\n')

    def test_startup_rejects_bad_ec_fields_without_consuming_token(self):
        conf = self.root / 'amuleapi.conf'
        token = self.root / 'amuleapi-ec-token'
        for fields, diagnostic in (('PasswordMD5=invalid', b'32 hexadecimal'),
                ('Password=plain\nPasswordMD5=' + 'a' * 32, b'cannot both be nonempty')):
            conf.write_text('[EC]\n' + fields + '\n')
            conf.chmod(0o600)
            token.write_text('a' * 32 + '\n')
            token.chmod(0o600)
            result = self.run_cli('--no-log-file')
            self.assertIn(diagnostic, result.stdout + result.stderr)
            self.assertEqual(token.read_text(), 'a' * 32 + '\n')

    @unittest.skipUnless(os.name == 'nt', 'native UTF-16 console input is Windows only')
    def test_terminal_utf16_backspace_and_invalid_surrogates(self):
        for scenario in ('emoji-delete', 'emoji-keep', 'invalid-surrogate'):
            before = self.credentials() if (self.root / 'amuleapi-passwords').exists() else None
            result = self.windows_terminal(scenario)
            if scenario == 'invalid-surrogate':
                self.assertEqual(result, 1)
                self.assertEqual(self.credentials(), before)
            else:
                self.assertEqual(result, 0)

    def windows_terminal(self, scenario):
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = subprocess.SW_HIDE
        result = subprocess.run([sys.executable,
            str(Path(__file__).with_name('WindowsPasswordConsoleSmoke.py')),
            BINARY, str(self.root), scenario], capture_output=True, timeout=30,
            creationflags=subprocess.CREATE_NEW_CONSOLE, startupinfo=startup)
        self.assert_ok(result)
        return json.loads(result.stdout)['returncode']

    def terminal(self, first, second=None, cancel=False, eof=False):
        if os.name == 'nt':
            scenario = 'cancel' if cancel else ('eof' if eof else
                       ('success' if first == second else 'mismatch'))
            return self.windows_terminal(scenario)
        import termios
        master, slave = os.openpty()
        self.addCleanup(os.close, master)
        self.addCleanup(os.close, slave)
        before = termios.tcgetattr(slave)
        proc = subprocess.Popen(self.command('--set-admin-pass-stdin'), stdin=slave,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(lambda: proc.kill() if proc.poll() is None else None)
        output = b''

        def wait_for(text):
            nonlocal output
            deadline = time.monotonic() + 5
            while text not in output:
                self.assertLess(time.monotonic(), deadline, output)
                if select.select([proc.stderr], [], [], 0.1)[0]:
                    part = os.read(proc.stderr.fileno(), 4096)
                    self.assertTrue(part, output)
                    output += part
            self.assertFalse(termios.tcgetattr(slave)[3] & termios.ECHO)

        wait_for(b'New admin password: ')
        if cancel:
            os.kill(proc.pid, signal.SIGINT)
        elif eof:
            os.write(master, b'\x04')
        else:
            os.write(master, first + b'\n')
            wait_for(b'Confirm password: ')
            os.write(master, second + b'\n')
        stdout, stderr = proc.communicate(timeout=5)
        self.assertEqual(termios.tcgetattr(slave), before)
        # Input should never be echoed to the PTY, nor printed in normal output.
        echoed = os.read(master, 4096) if select.select([master], [], [], 0)[0] else b''
        self.assertNotIn(first, echoed + output + stdout + stderr)
        return proc.returncode

    def test_terminal_confirm_and_restore_echo(self):
        self.assertEqual(self.terminal(b'terminal-secret', b'terminal-secret'), 0)
        self.assertTrue((self.root / 'amuleapi-passwords').exists())

    def test_terminal_mismatch_writes_nothing(self):
        self.assertNotEqual(self.terminal(b'terminal-secret', b'other-secret'), 0)
        self.assertEqual(list(self.root.iterdir()), [])

    def test_terminal_interrupt_restores_echo(self):
        self.assertNotEqual(self.terminal(b'not-typed', cancel=True), 0)
        self.assertEqual(list(self.root.iterdir()), [])

    def test_terminal_eof_restores_echo(self):
        self.assertNotEqual(self.terminal(b'not-typed', eof=True), 0)
        self.assertEqual(list(self.root.iterdir()), [])


if __name__ == '__main__':
    unittest.main(verbosity=2)
