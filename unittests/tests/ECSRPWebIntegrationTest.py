#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team. SPDX-License-Identifier: GPL-2.0-or-later
"""Core-spawned amuleweb after verifier migration, with independent API tokens."""
import hashlib
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

from AllSearchIntegrationTest import EC, connect_daemon, free_ports, retry_port_collisions, stop_daemon, tag
from DaemonConfig import write_amule_conf
import ECWire as W


def wait_for(predicate, message, timeout=15):
    deadline = time.monotonic() + timeout
    while not predicate():
        assert time.monotonic() < deadline, message() if callable(message) else message
        time.sleep(0.05)


def run(daemon_binary, web_binary, api_binary=None):
    with tempfile.TemporaryDirectory(prefix='amule-srp-web-') as temporary:
        root = Path(temporary)
        ec_port, web_port, api_port, peer = free_ports(4, peer_index=3)
        wrapper = root / 'start-web'
        wrapper.write_text(
            '#!' + sys.executable + '\nimport os\nfrom pathlib import Path\nimport sys\n'
            'root = Path(__file__).parent\n'
            'token = root / "amuleweb-ec-token"\n'
            'assert token.stat().st_mode & 0o777 == 0o600\n'
            '(root / "captured-token").write_bytes(token.read_bytes())\n'
            '(root / "child-pid").write_text(str(os.getpid()))\n'
            'os.execv(' + repr(web_binary) + ', [' + repr(web_binary) + '] + sys.argv[1:])\n')
        wrapper.chmod(0o700)
        sections = {'WebServer': {'Enabled': 1, 'Path': wrapper, 'Port': web_port,
                                  'Template': 'default',
                                  'Password': hashlib.md5(b'web-admin').hexdigest()}}
        if api_binary:
            sections['AmuleApi'] = {'Enabled': 1, 'Path': api_binary,
                                    'BindAddress': '127.0.0.1', 'HttpPort': api_port}
            (root / 'amuleapi.conf').write_text(
                f'[Server]\nBindAddress=127.0.0.1\nPort={api_port}\n'
                f'[EC]\nHost=127.0.0.1\nPort={ec_port}\nPassword=deliberately-wrong\n')
            (root / 'amuleapi.conf').chmod(0o600)
        write_amule_conf(root, ec_port, Port=peer, UDPEnable=0, sections=sections)
        (root / 'webserver').mkdir()
        (root / 'webserver/default').symlink_to(
            Path(__file__).resolve().parents[2] / 'src/webserver/default', target_is_directory=True)
        env = dict(os.environ, HOME=temporary, XDG_CONFIG_HOME=str(root / 'xdg'), LC_ALL='C.UTF-8')
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

        def http_ready(port, path='/'):
            try:
                with opener.open(f'http://127.0.0.1:{port}{path}', timeout=1) as response:
                    return response.status == 200 and bool(response.read())
            except (OSError, urllib.error.URLError):
                return False

        def diagnostics(message):
            logs = [path.read_text(errors='replace')[-5000:]
                    for path in (root / 'console', root / 'logfile', root / 'amuleapi.log')
                    if path.exists()]
            return message + '\n' + '\n'.join(logs)

        old_token = None
        for restart in range(3):
            if restart == 2:
                wrapper.write_text(wrapper.read_text().split('os.execv(', 1)[0] + 'sys.exit(0)\n')
                (root / 'child-pid').unlink()
            with (root / 'console').open('w') as log:
                daemon = subprocess.Popen([daemon_binary, '-c', temporary, '--disable-fatal'],
                                          env=env, stdout=log, stderr=log)
                try:
                    ec = connect_daemon(daemon, ec_port, root, peer)
                    ec.sock.close()
                    if restart == 2:
                        wait_for(lambda: (root / 'child-pid').exists(), 'helper did not start')
                        child_pid = int((root / 'child-pid').read_text())
                        wait_for(lambda: f"with pid '{child_pid}' has finished" in (root / 'logfile').read_text(),
                                 'core did not observe early helper exit')
                        assert not (root / 'amuleweb-ec-token').exists(), 'early exit left unread token'
                        token = bytes.fromhex((root / 'captured-token').read_text().strip())
                        with socket.create_connection(('127.0.0.1', ec_port), timeout=10) as sock:
                            _, op, _ = W.authenticate(sock, token, credential=2)
                            assert op == W.C['EC_OP_AUTH_FAIL'], 'early helper exit did not revoke token'
                        print('PASS: helper exit before consuming its token removes and revokes it')
                        continue
                    wait_for(lambda: http_ready(web_port), lambda: diagnostics('spawned amuleweb did not serve HTTP'))
                    assert '\nECPassword=' not in (root / 'amule.conf').read_text()
                    assert not (root / 'amuleweb-ec-token').exists(), 'web token was not consumed'
                    token = bytes.fromhex((root / 'captured-token').read_text().strip())
                    assert token != old_token, 'web child reused a token across core restarts'
                    old_token = token
                    if api_binary:
                        wait_for(lambda: http_ready(api_port, '/api/v1/health'), lambda: diagnostics('spawned API failed'))
                        assert not (root / 'amuleapi-ec-token').exists(), 'API token was not consumed'
                    # A web token has its own selector, cannot log in as the API, and
                    # remains usable after the API's unrelated token is issued.
                    for credential, expected in ((2, 'EC_OP_AUTH_OK'), (1, 'EC_OP_AUTH_FAIL')):
                        with socket.create_connection(('127.0.0.1', ec_port), timeout=10) as sock:
                            _, op, _ = W.authenticate(sock, token, credential=credential)
                            assert op == W.C[expected], 'child credential selectors were mixed'
                    with socket.create_connection(('127.0.0.1',ec_port),timeout=10) as sock:
                        encrypted,op,_=W.authenticate(sock,token,credential=2)
                        assert op==W.C['EC_OP_AUTH_OK']
                        # A rejected password edit must not apply unrelated prefs either.
                        password_tag=W.tag('EC_TAG_EXTERNALCONN_PASSWD',hashlib.md5(b'evil').digest(),9)
                        group=tag(W.C['EC_TAG_PREFS_REMOTECTRL'],children=[password_tag])
                        editor = EC(ec_port)
                        full = tag(W.C['EC_TAG_DETAIL_LEVEL'], bytes([W.C['EC_DETAIL_FULL']]), 2)
                        select = tag(W.C['EC_TAG_SELECT_PREFS'], struct.pack('!I', W.C['EC_PREFS_GENERAL']), 4)
                        def nickname():
                            _, prefs = editor.call(W.C['EC_OP_GET_PREFERENCES'], [full, select])
                            return prefs[W.C['EC_TAG_PREFS_GENERAL']][1][W.C['EC_TAG_USER_NICK']][0]
                        original_nick = nickname()
                        general = tag(W.C['EC_TAG_PREFS_GENERAL'], children=[
                            tag(W.C['EC_TAG_USER_NICK'], b'token-must-not-apply\0', 6)])
                        encrypted.sendall(W.packet('EC_OP_SET_PREFERENCES',[group, general]))
                        assert W.read(encrypted)[0]==W.C['EC_OP_FAILED']
                        assert nickname() == original_nick, 'token password edit applied unrelated preferences'
                        editor.sock.close()
                    EC_salt=[]
                    for selector in (1,2):
                        with socket.create_connection(('127.0.0.1',ec_port),timeout=10) as sock:
                            W.authenticate(sock,token,credential=selector,challenge=lambda tags: EC_salt.append(tags[W.C['EC_TAG_SRP_SALT']]))
                    assert EC_salt[0]==EC_salt[1], 'salt reveals live token selector'
                    if api_binary:
                        assert http_ready(api_port, '/api/v1/health'), 'web login disrupted API'
                    # Revocation must also reject a proof prepared before child exit.
                    def terminate_child():
                        child_pid = int((root / 'child-pid').read_text())
                        os.kill(child_pid, signal.SIGTERM)
                        wait_for(lambda: f"with pid '{child_pid}' has finished" in (root / 'logfile').read_text(),
                                 'core did not observe amuleweb exit')
                    with socket.create_connection(('127.0.0.1', ec_port), timeout=10) as sock:
                        _, op, _ = W.authenticate(sock, token, credential=2,
                                                 before_proof=terminate_child)
                        assert op == W.C['EC_OP_AUTH_FAIL'], 'revoked in-flight web token accepted'
                    with socket.create_connection(('127.0.0.1', ec_port), timeout=10) as sock:
                        _, op, _ = W.authenticate(sock, token, credential=2)
                        assert op == W.C['EC_OP_AUTH_FAIL'], 'exited child retained its credential'
                    if api_binary:
                        assert http_ready(api_port, '/api/v1/health'), 'web revocation disrupted API'
                finally:
                    stop_daemon(daemon)
            assert not (root / 'amuleweb-ec-token').exists(), 'shutdown retained web token'
            print('PASS: web startup, token isolation and revocation; restart', restart,
                  'with API' if api_binary else 'without API')

        # Standalone use of the core-config mode must fail promptly without a
        # token rather than prompting indefinitely on redirected stdin.
        for malformed in (False, True):
            if malformed:
                (root / 'amuleweb-ec-token').write_text('not-a-valid-token')
            with tempfile.TemporaryFile() as output:
                child = subprocess.Popen([web_binary, '--amule-config-file=' + str(root / 'amule.conf')],
                                         env=env, stdin=subprocess.PIPE, stdout=output, stderr=output)
                try:
                    child.wait(timeout=5)
                    output.seek(0)
                    assert b'No EC credential' in output.read(), 'missing token had no useful diagnostic'
                    assert not (root / 'amuleweb-ec-token').exists(), 'malformed token was not removed'
                finally:
                    if child.poll() is None:
                        child.kill()
                        child.wait()
                    child.stdin.close()
        print('PASS: absent/malformed child token fails without a password prompt')


if __name__ == '__main__':
    binaries = [str(Path(p).resolve()) for p in sys.argv[1:]]
    retry_port_collisions(lambda: run(*binaries[:2]))
    if len(binaries) == 3:
        retry_port_collisions(lambda: run(*binaries))
