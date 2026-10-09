#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check logfile selection and EC read/reset with isolated, offline daemon instances."""
import ctypes
import hashlib
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time

from AllSearchIntegrationTest import C, EC, string


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def stop_forked_daemon(pid):
    os.kill(pid, signal.SIGTERM)
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        ended, status = os.waitpid(pid, os.WNOHANG)
        if ended:
            assert os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0, status
            return
        time.sleep(0.05)
    os.kill(pid, signal.SIGKILL)
    os.waitpid(pid, 0)
    raise AssertionError('forked daemon did not shut down cleanly')


def write_config(root, setting=None):
    """Write an offline amule.conf with EC on a free port, and return that port."""
    port = free_port()
    config = f'''[eMule]
Nick=logfile-regression
FirstRunWizardDone=1
Port={free_port()}
UDPEnable=0
Address=127.0.0.1
ConnectToKad=0
ConnectToED2K=0
NewVersionCheck=0
Reconnect=0
Serverlist=0
Ed2kServersUrl=
KadNodesUrl=
TempDir={root}/Temp
IncomingDir={root}/Incoming
# The checks match English log lines; LC_ALL alone does not override a translated LANG.
Language=en_US
'''
    if setting is not None:
        config += f'LogFilePath={setting}\n'
    config += f'''[ExternalConnect]
AcceptExternalConnections=1
ECAddress=127.0.0.1
ECPort={port}
ECPassword={hashlib.md5(b'regression').hexdigest()}
'''
    (root / 'amule.conf').write_text(config)
    return port


def daemon_env(root):
    env = dict(os.environ, HOME=str(root), XDG_CONFIG_HOME=str(root / 'xdg'))
    env.pop('LD_PRELOAD', None)
    env['LC_ALL'] = 'C.UTF-8'
    return env


def run(binary, root, setting=None, override=None, expected=None, failure=False,
        backup_failure=False, full_daemon=False, reset_failure=False):
    root.mkdir()
    port = write_config(root, setting)
    args = [binary, '-c', str(root), '--disable-fatal']
    if override is not None:
        args += [f'--log-file={override}']
    env = daemon_env(root)
    pidfile = root / 'daemon.pid'
    if full_daemon:
        args += ['--full-daemon', '--pid-file=' + str(pidfile)]
    if backup_failure:
        expected.write_bytes(b'previous log must survive a failed backup\n')
        backup = Path(str(expected) + '.bak')
        backup.mkdir()
        (backup / 'keep').write_bytes(b'existing backup directory content\n')
    # Run twice to verify startup backup, and read/reset through EC each time.
    for restart in range(1 if failure else 2):
        if full_daemon:
            pidfile.unlink(missing_ok=True)
        with (root / 'console.log').open('w') as console:
            proc = subprocess.Popen(args, stdout=console, stderr=console, env=env)
            ec = None
            daemon_pid = None
            daemon_reaped = False
            try:
                if full_daemon and not failure:
                    assert proc.wait() == 0
                if failure:
                    assert proc.wait() != 0
                    error = (root / 'console.log').read_text()
                    assert 'ERROR:' in error and 'log file' in error, error
                    if expected != root / 'logfile':
                        assert not (root / 'logfile').exists(), 'silently used default path'
                    return
                # Password hashing can be slow on some architectures. Like
                # connect_daemon(), wait while the daemon lives; ctest/the caller
                # owns the overall deadline. A forked launcher's success is not
                # evidence that its daemon child is still alive.
                while ec is None:
                    if full_daemon:
                        if pidfile.exists():
                            pid = pidfile.read_text().strip()
                            if pid:
                                daemon_pid = int(pid)
                        try:
                            ended, _ = os.waitpid(daemon_pid or -1, os.WNOHANG)
                        except ChildProcessError:
                            ended = True
                        if ended:
                            daemon_reaped = True
                            raise AssertionError((root / 'console.log').read_text())
                    elif proc.poll() is not None:
                        raise AssertionError((root / 'console.log').read_text())
                    try:
                        ec = EC(port)
                    except ConnectionRefusedError:
                        time.sleep(0.1)
                if full_daemon:
                    assert daemon_pid is not None and daemon_pid != proc.pid
                assert expected.is_file(), expected
                if expected != root / 'logfile':
                    assert not (root / 'logfile').exists()
                if backup_failure:
                    prefix = previous if restart else b'previous log must survive a failed backup\n'
                    assert expected.read_bytes().startswith(prefix)
                    assert (backup / 'keep').read_bytes() == b'existing backup directory content\n'
                    assert b'appending to the existing log' in expected.read_bytes()
                elif restart:
                    assert Path(str(expected) + '.bak').read_bytes() == previous
                op, tags = ec.call(C['EC_OP_GET_LOG'])
                text = tags[C['EC_TAG_STRING']][0].decode()
                assert op == C['EC_OP_LOG'] and 'ERROR: can\'t open logfile' not in text, text
                assert 'aMule' in text, text
                # An old-file marker verifies reset truncates the selected file.
                assert ec.call(C['EC_OP_ADDLOGLINE'], [
                    string(C['EC_TAG_STRING'], 'logfile-regression-before-reset')
                ])[0] == C['EC_OP_NOOP']
                _, tags = ec.call(C['EC_OP_GET_LOG'])
                assert b'logfile-regression-before-reset' in tags[C['EC_TAG_STRING']][0]
                if reset_failure:
                    expected.chmod(0o400)
                    try:
                        assert ec.call(C['EC_OP_RESET_LOG'])[0] == C['EC_OP_NOOP']
                        # The log is still readable after a failed reopen. The next
                        # reset must retry the selected path after permissions recover.
                        _, tags = ec.call(C['EC_OP_GET_LOG'])
                        assert b'logfile-regression-before-reset' in tags[C['EC_TAG_STRING']][0]
                    finally:
                        expected.chmod(0o600)
                assert ec.call(C['EC_OP_RESET_LOG'])[0] == C['EC_OP_NOOP']
                _, tags = ec.call(C['EC_OP_GET_LOG'])
                text = tags[C['EC_TAG_STRING']][0]
                assert b'logfile-regression-before-reset' not in text, text
                assert b'Log has been reset' in text, text
            finally:
                if ec:
                    ec.sock.close()
                if full_daemon and not daemon_reaped:
                    if daemon_pid is None and pidfile.exists():
                        daemon_pid = int(pidfile.read_text())
                    if daemon_pid is not None:
                        stop_forked_daemon(daemon_pid)
                if proc.poll() is None:
                    proc.terminate()
                try:
                    proc.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
                if full_daemon and failure:
                    deadline = time.monotonic() + 5
                    while time.monotonic() < deadline:
                        try:
                            ended, _ = os.waitpid(-1, os.WNOHANG)
                        except ChildProcessError:
                            break
                        if not ended:
                            time.sleep(0.05)
                    else:
                        raise AssertionError('failed startup left a child running')
        previous = expected.read_bytes()
        saved = (root / 'amule.conf').read_text()
        assert f'LogFilePath={setting or ""}\n' in saved, saved


def run_second_instance(binary, root):
    """A second amuled -f on a config another daemon holds must say why it stopped."""
    root.mkdir()
    port = write_config(root)
    env = daemon_env(root)
    with (root / 'first.log').open('w') as out:
        first = subprocess.Popen([binary, '-c', str(root), '--disable-fatal'],
                                 stdout=out, stderr=out, env=env)
        ec = None
        try:
            while ec is None:
                if first.poll() is not None:
                    raise AssertionError((root / 'first.log').read_text())
                try:
                    ec = EC(port)
                except ConnectionRefusedError:
                    time.sleep(0.1)
            second = subprocess.run([binary, '-c', str(root), '--disable-fatal', '--full-daemon'],
                                    capture_output=True, text=True, env=env)
            assert second.returncode != 0, second
            # The launcher relays what the child logged before it stopped, which names the
            # holder, instead of a generic startup failure.
            assert 'already running' in second.stderr, second.stderr
            assert 'run it without -f' not in second.stderr, second.stderr
        finally:
            if ec:
                ec.sock.close()
            first.terminate()
            first.wait()
    # The subreaper adopted the second daemon's child; it exits right after reporting.
    while True:
        try:
            os.waitpid(-1, 0)
        except ChildProcessError:
            break


def main():
    binary = str(Path(sys.argv[1]).resolve())
    # Adopt daemonized children so cleanup can wait for them instead of leaving zombies.
    if sys.platform.startswith('linux'):
        assert ctypes.CDLL(None).prctl(36, 1, 0, 0, 0) == 0  # PR_SET_CHILD_SUBREAPER
    with tempfile.TemporaryDirectory(prefix='amule-logfile-') as scratch:
        base = Path(scratch)
        run(binary, base / 'default', expected=base / 'default/logfile')
        run(binary, base / 'empty', setting='', expected=base / 'empty/logfile')
        logs = base / 'logs with spaces'
        logs.mkdir()
        run(binary, base / 'configured', setting=str(logs / 'configured.log'),
            expected=logs / 'configured.log')
        run(binary, base / 'relative', setting='relative.log',
            expected=base / 'relative/relative.log')
        run(binary, base / 'override', setting=str(logs / 'unused.log'),
            override=str(logs / 'override.log'), expected=logs / 'override.log')
        assert not (logs / 'unused.log').exists()
        run(binary, base / 'cli-relative', override='cli.log',
            expected=base / 'cli-relative/cli.log')
        run(binary, base / 'missing', setting=str(base / 'no-directory/logfile'), failure=True)
        run(binary, base / 'directory', override=str(logs), failure=True)
        run(binary, base / 'backup-default', expected=base / 'backup-default/logfile',
            backup_failure=True)
        run(binary, base / 'backup-custom', setting=str(logs / 'preserved.log'),
            expected=logs / 'preserved.log', backup_failure=True)
        if os.geteuid() != 0:
            run(binary, base / 'reset-recovery', setting='selected.log',
                expected=base / 'reset-recovery/selected.log', reset_failure=True)
        if sys.platform.startswith('linux'):
            run(binary, base / 'fork-missing', setting=str(base / 'missing-fork/logfile'),
                failure=True, full_daemon=True)
            run(binary, base / 'fork-directory', override=str(logs),
                failure=True, full_daemon=True)
            run(binary, base / 'fork-backup', setting='appended.log',
                expected=base / 'fork-backup/appended.log', backup_failure=True, full_daemon=True)
            run(binary, base / 'fork-relative', setting='daemon.log',
                expected=base / 'fork-relative/daemon.log', full_daemon=True)
            run(binary, base / 'fork-override', setting='unused.log',
                override=str(logs / 'daemon-日志.log'), expected=logs / 'daemon-日志.log',
                full_daemon=True)
            assert not (base / 'fork-override/unused.log').exists()
            run_second_instance(binary, base / 'fork-second-instance')
        # /proc is unwritable even when the test runner is root.
        if Path('/proc').is_dir():
            run(binary, base / 'unwritable', setting='/proc/amule-logfile-test', failure=True)
    print('Logfile selection, rotation, persistence, EC read/reset and error checks passed')


if __name__ == '__main__':
    main()
