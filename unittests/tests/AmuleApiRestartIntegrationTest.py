#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise API child supervision and fresh EC credentials with an offline core."""
import hashlib
import json
import os
from pathlib import Path
import shlex
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import time

from ECWire import authenticate  # Preflight in the parent before starting fixtures.
from AllSearchIntegrationTest import (C, EC, check_daemon_ports, integer,
                                      retry_port_collisions, string, tag)
from LogfilePathIntegrationTest import daemon_env, write_config


# Matches the opt-in integration-build clock in CamuleApp::OnInit().
FIRST_RETRY = 0.5
SECOND_RETRY = 1.0
POLL_INTERVAL = 0.05


def authenticate_token(host, port, token):
    ec = EC.__new__(EC)
    ec.sock = socket.create_connection((host, port), timeout=10)
    try:
        ec.sock, op, _ = authenticate(ec.sock, bytes.fromhex(token), credential=1)
        return op
    finally:
        ec.sock.close()


def helper():
    args = dict(arg[2:].split('=', 1) for arg in sys.argv[1:] if arg.startswith('--'))
    root = Path(args['config-dir'])
    scenario = (root / 'scenario').read_text()
    token_file = root / 'amuleapi-ec-token'
    mode = stat.S_IMODE(token_file.stat().st_mode)
    token = token_file.read_text().strip()
    journal = root / 'launches.jsonl'
    count = len(journal.read_text().splitlines()) if journal.exists() else 0
    unread = scenario == 'unread' and count == 0
    if not unread:
        token_file.unlink()
        assert authenticate_token(args['host'], int(args['port']), token) == C['EC_OP_AUTH_OK']
    # The private fixture keeps the retired secret to verify revocation after exit.
    with (root / 'retired-token').open('w') as output:
        output.write(token)
    (root / 'retired-token').chmod(0o600)
    with journal.open('a') as output:
        output.write(json.dumps({'pid': os.getpid(), 'time': time.monotonic(),
                                 'token_hash': hashlib.sha256(token.encode()).hexdigest(),
                                 'mode': mode}) + '\n')
    if count == 0 and scenario != 'active-shutdown':
        # Let the parent inspect startup and arrange a missing executable before
        # starting the short retry clock, even on a heavily loaded runner.
        deadline = time.monotonic() + 90
        while not (root / 'allow-exit').exists():
            assert time.monotonic() < deadline, 'parent did not release helper'
            time.sleep(POLL_INTERVAL)
    if scenario == 'verbose' and count == 0:
        # Fill both streams beyond pipe capacity before reporting the failed exit.
        sys.stdout.write('stdout regression ' * 16384 + '\n')
        sys.stdout.flush()
        sys.stderr.write('stderr regression ' * 16384 + '\n')
        sys.stderr.flush()
    while scenario == 'active-shutdown':
        time.sleep(1)
    return 1 if count == 0 else 0


def launches(root):
    journal = root / 'launches.jsonl'
    # A reader can overlap the helper's append. Wait for a complete JSON line.
    lines = journal.read_text().splitlines(keepends=True) if journal.exists() else []
    return [json.loads(line) for line in lines if line.endswith('\n')]


def wait_until(proc, root, predicate, timeout=90, *, daemon_stopped=False):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        console = (root / 'console.log').read_text(errors='replace')
        assert daemon_stopped or proc.poll() is None, console
        assert 'but the amuleapi binary cannot be run' not in console, 'API launch rejected the profile path'
        time.sleep(POLL_INTERVAL)
    raise AssertionError((root / 'console.log').read_text(errors='replace'))


def assert_no_restart(proc, root, count, duration):
    # Absence is not an event: observe beyond the pending backoff plus timer
    # slack, checking throughout rather than sleeping for production seconds.
    deadline = time.monotonic() + duration

    def observed():
        assert proc.poll() is None, (root / 'console.log').read_text(errors='replace')
        assert len(launches(root)) == count, 'unexpected API restart'
        return time.monotonic() >= deadline

    wait_until(proc, root, observed, timeout=duration + 10)


def child_exited(pid):
    try:
        return (Path('/proc') / str(pid) / 'stat').read_text().rpartition(')')[2].split()[0] == 'Z'
    except FileNotFoundError:
        return True


def run(binary, root, scenario):
    root.mkdir()
    (root / 'scenario').write_text(scenario)
    ec_port, peer_port = write_config(root)
    helper_path = root / 'api-helper'
    helper_script = ('#!/bin/sh\nexec ' + shlex.quote(sys.executable) + ' ' +
                     shlex.quote(str(Path(__file__).resolve())) + ' "$@"\n')
    helper_path.write_text(helper_script)
    helper_path.chmod(0o700)
    with (root / 'amule.conf').open('a') as config:
        config.write(f'\n[AmuleApi]\nEnabled=1\nPath={helper_path}\nBindAddress=127.0.0.1\n')
    with (root / 'console.log').open('w') as console:
        proc = subprocess.Popen([binary, '-c', str(root), '--disable-fatal'],
                                stdout=console, stderr=console, env=dict(daemon_env(root), AMULE_TEST_FAST_API_RESTART='1'))
        try:
            wait_until(proc, root, lambda: len(launches(root)) == 1)
            check_daemon_ports(root, ec_port, peer_port)
            if scenario == 'missing':
                helper_path.unlink()
            if scenario != 'active-shutdown':
                released_at = time.monotonic()
                if scenario == 'unread':
                    token = (root / 'retired-token').read_text().strip()
                    def revoke_pending_token():
                        (root / 'allow-exit').touch()
                        wait_until(proc, root, lambda: 'amuleapi will restart in 0 seconds.' in
                                   (root / 'logfile').read_text(errors='replace'))
                    with socket.create_connection(('127.0.0.1', ec_port), timeout=15) as pending:
                        _, op, _ = authenticate(pending, bytes.fromhex(token), credential=1,
                                                before_proof=revoke_pending_token)
                        assert op == C['EC_OP_AUTH_FAIL'], 'pending proof accepted after token revocation'
                else:
                    (root / 'allow-exit').touch()
                wait_until(proc, root, lambda: 'amuleapi will restart in 0 seconds.' in
                           (root / 'logfile').read_text(errors='replace'),
                           timeout=10 if scenario == 'verbose' else 90)
            if scenario == 'unread':
                assert not (root / 'amuleapi-ec-token').exists(), 'dead child left a live token file'
                token = (root / 'retired-token').read_text()
                assert authenticate_token('127.0.0.1', ec_port, token) == C['EC_OP_AUTH_FAIL'], 'dead child token still accepted'
            if scenario in ('shutdown', 'active-shutdown'):
                proc.terminate()
                assert proc.wait(timeout=20) == 0
                assert len(launches(root)) == 1, 'child restarted during shutdown'
                if scenario == 'active-shutdown':
                    wait_until(proc, root, lambda: child_exited(launches(root)[0]['pid']),
                               timeout=20, daemon_stopped=True)
            elif scenario == 'disabled':
                ec = EC(ec_port)
                try:
                    assert ec.call(C['EC_OP_SET_PREFERENCES'], [
                        tag(C['EC_TAG_PREFS_REMOTECTRL'], children=[
                            integer(C['EC_TAG_AMULEAPI_AUTORUN'], 0)]),
                    ])[0] == C['EC_OP_NOOP']
                    assert_no_restart(proc, root, 1, FIRST_RETRY + 0.5)
                    assert len(launches(root)) == 1, 'disabled API restarted'
                    # Re-enabling must not revive an already cancelled retry.
                    assert ec.call(C['EC_OP_SET_PREFERENCES'], [
                        tag(C['EC_TAG_PREFS_REMOTECTRL'], children=[
                            integer(C['EC_TAG_AMULEAPI_AUTORUN'], 1)]),
                    ])[0] == C['EC_OP_NOOP']
                    assert_no_restart(proc, root, 1, SECOND_RETRY + 0.5)
                    assert len(launches(root)) == 1, 'cancelled retry revived'
                finally:
                    ec.sock.close()
            else:
                if scenario == 'missing':
                    # wxWidgets may report a PID before exec fails, then deliver
                    # the failure through OnTerminate instead of returning zero.
                    wait_until(proc, root, lambda: 'amuleapi will restart in 1 seconds.' in
                               (root / 'logfile').read_text(errors='replace'))
                    assert not (root / 'amuleapi-ec-token').exists(), 'failed launch left a token file'
                    assert 'amuleapi will restart in 1 seconds.' in (root / 'logfile').read_text()
                    helper_path.write_text(helper_script)
                    helper_path.chmod(0o700)
                wait_until(proc, root, lambda: len(launches(root)) == 2)
                records = launches(root)
                assert records[0]['token_hash'] != records[1]['token_hash'], 'reused EC token'
                assert all(record['mode'] == 0o600 for record in records)
                delay = FIRST_RETRY + SECOND_RETRY if scenario == 'missing' else FIRST_RETRY
                assert records[1]['time'] - released_at >= delay, 'restart skipped delay'
                assert not (root / 'amuleapi-ec-token').exists()
                # Token revocation confirms the core processed the clean exit.
                token = (root / 'retired-token').read_text()
                wait_until(proc, root, lambda: authenticate_token('127.0.0.1', ec_port, token) == C['EC_OP_AUTH_FAIL'])
                # If clean exit incorrectly armed a retry it would fire by the
                # next backoff (2 s after the missing-executable failure).
                next_retry = 2 * SECOND_RETRY if scenario == 'missing' else SECOND_RETRY
                assert_no_restart(proc, root, 2, next_retry + 0.5)
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()


if __name__ == '__main__':
    if any(arg.startswith('--config-dir=') for arg in sys.argv[1:]):
        sys.exit(helper())
    with tempfile.TemporaryDirectory(prefix='amule-api-restart-') as directory:
        root = Path(directory)
        scenarios = sys.argv[2:] or ['unread', 'restart', 'quoted', 'missing', 'verbose', 'disabled', 'shutdown', 'active-shutdown']
        for scenario in scenarios:
            # Quoted executable and profile paths exercise argument boundaries.
            name = "profile '北' with spaces" if scenario == 'quoted' else scenario
            def attempt():
                with tempfile.TemporaryDirectory(prefix=name + '-', dir=root) as case:
                    run(str(Path(sys.argv[1]).resolve()), Path(case) / name, scenario)
            retry_port_collisions(attempt)
    print('API supervision regression scenarios passed')
