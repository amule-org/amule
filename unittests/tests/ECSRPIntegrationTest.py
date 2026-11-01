#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team. SPDX-License-Identifier: GPL-2.0-or-later
"""Offline migration, production client logins and active split-channel MITM."""
import hashlib
import os
from pathlib import Path
import socket
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading
import time

from AllSearchIntegrationTest import (C, EC, tag, connect_daemon, free_ports, running_daemon,
                                      stop_daemon)
from DaemonConfig import daemon_env, write_amule_conf
from ConnectorConfigPathTest import read_config
import ECWire as W

DAEMON_ARGS = () if os.name == 'nt' else ('--disable-fatal',)

def wire_tags(fields):
    names = {v:k for k,v in C.items() if k.startswith('EC_TAG_')}
    result=[]
    for code,value in fields.items():
        name=names[code]
        kind=6 if name in ('EC_TAG_CLIENT_NAME','EC_TAG_CLIENT_VERSION') else (
            3 if name=='EC_TAG_PROTOCOL_VERSION' else (
                9 if name=='EC_TAG_VERSION_ID' else (2 if name=='EC_TAG_AEAD_CIPHER' else 1)))
        result.append(W.tag(name,value,kind))
    return result

class Relay(socketserver.BaseRequestHandler):
    def handle(self):
        try:
            with socket.create_connection(('127.0.0.1',self.server.target),timeout=15) as upstream:
                op,req=W.read(self.request); assert op==C['EC_OP_AUTH_REQ']
                cp=req[C['EC_TAG_AEAD_CLIENT_PUBKEY']];cn=req[C['EC_TAG_AEAD_CLIENT_NONCE']]
                offered=req[C['EC_TAG_CAN_AEAD']]
                to_server=W.X25519PrivateKey.generate();ts_pub=to_server.public_key().public_bytes(W.serialization.Encoding.Raw,W.serialization.PublicFormat.Raw)
                req[C['EC_TAG_AEAD_CLIENT_PUBKEY']]=ts_pub
                upstream.sendall(W.packet('EC_OP_AUTH_REQ',wire_tags(req)))
                op,reply=W.read(upstream);assert op==C['EC_OP_AUTH_SALT']
                sp=reply[C['EC_TAG_AEAD_SERVER_PUBKEY']];sn=reply[C['EC_TAG_AEAD_SERVER_NONCE']]
                cipher=int.from_bytes(reply[C['EC_TAG_AEAD_CIPHER']],'big')
                to_client=W.X25519PrivateKey.generate();tc_pub=to_client.public_key().public_bytes(W.serialization.Encoding.Raw,W.serialization.PublicFormat.Raw)
                reply[C['EC_TAG_AEAD_SERVER_PUBKEY']]=tc_pub
                self.request.sendall(W.packet('EC_OP_AUTH_SALT',wire_tags(reply)))
                prefix=bytes([len(offered)])+offered+bytes([cipher])+cn+sn
                client_leg=W.EncryptedSocket(self.request,to_client.exchange(W.X25519PublicKey.from_public_bytes(cp)),cn,sn,prefix+cp+tc_pub,cipher,True)
                server_leg=W.EncryptedSocket(upstream,to_server.exchange(W.X25519PublicKey.from_public_bytes(sp)),cn,sn,prefix+ts_pub+sp,cipher)
                op,proof=W.read(client_leg);assert op==C['EC_OP_AUTH_PASSWD']
                self.server.observed=proof[C['EC_TAG_SRP_M1']]
                server_leg.sendall(W.packet('EC_OP_AUTH_PASSWD',wire_tags(proof)))
                op,result=W.read(server_leg)
                assert op==C['EC_OP_AUTH_FAIL'],(op,result)
                # AUTH_FAIL carries a string, not an SRP field.
                payload=[W.tag('EC_TAG_STRING',result[C['EC_TAG_STRING']],6)]
                client_leg.sendall(W.packet('EC_OP_AUTH_FAIL',payload))
        except BaseException as exc:
            self.server.error=exc

class FixtureServer(socketserver.ThreadingTCPServer):
    daemon_threads=True
    allow_reuse_address=True

class BadServerProof(socketserver.BaseRequestHandler):
    def handle(self):
        try:
            def evidence(proof):
                if self.server.plaintext:
                    return [W.tag('EC_TAG_SRP_M2', proof)]
                if self.server.missing:
                    return []
                return [W.tag('EC_TAG_SRP_M2', bytes([proof[0] ^ 1]) + proof[1:])]
            encrypted = W.accept(self.request, server_proof=evidence,
                                 plaintext=self.server.plaintext)
            self.request.settimeout(3)
            try:
                self.server.extra = encrypted.recv(8)
            except (socket.timeout, EOFError, ConnectionResetError):
                self.server.extra = b''
        except BaseException as exc:
            self.server.error = exc

class OldDaemon(socketserver.BaseRequestHandler):
    def handle(self):
        _,req=W.read(self.request)
        # Retired numeric salt tag; deliberately no SRP B.
        salt=struct.pack('!HBIQ',0x000b*2,5,8,123)
        if getattr(self.server,'bad_profile',False):
            fields=[W.tag('EC_TAG_SRP_B',W.pad(5)),W.tag('EC_TAG_SRP_SALT',bytes(16)),
                    W.tag('EC_TAG_SRP_PROFILE',bytes([1,31,8,1])),
                    W.tag('EC_TAG_AEAD_CIPHER',b'\x01',2),
                    W.tag('EC_TAG_AEAD_SERVER_NONCE',os.urandom(32)),
                    W.tag('EC_TAG_AEAD_SERVER_PUBKEY',W.X25519PrivateKey.generate().public_key().public_bytes(W.serialization.Encoding.Raw,W.serialization.PublicFormat.Raw))]
            self.request.sendall(W.packet('EC_OP_AUTH_SALT',fields))
        else:
            body=bytes([C['EC_OP_AUTH_SALT']])+struct.pack('!H',1)+salt
            self.request.sendall(struct.pack('!II',0x20,len(body))+body)
        self.request.settimeout(3)
        try: self.server.extra=self.request.recv(4096)
        except (socket.timeout,ConnectionResetError): self.server.extra=b''

class PlaintextReply(socketserver.BaseRequestHandler):
    def handle(self):
        try:
            encrypted = W.accept(self.request)
            W.read(encrypted)  # The client's first authenticated command.
            self.request.sendall(W.packet('EC_OP_FAILED', [
                W.tag('EC_TAG_STRING', b'injected plaintext response\0', 6)]))
        except BaseException as exc:
            self.server.error = exc

def rate_limit_boundary(daemon, cmd, async_probe, root, env):
    """An admitted challenge may finish even if its reservation arms the lockout."""
    class StopChallenge(Exception):
        pass

    for threshold in (1, 2):
        directory = root / ('rate-limit-' + str(threshold))
        directory.mkdir()
        port, peer = free_ports(2, peer_index=1)
        write_amule_conf(directory, port, Port=peer, UDPEnable=0)
        config = directory / 'amule.conf'
        config.write_text(config.read_text().replace(
            '[ExternalConnect]\n',
            f'[ExternalConnect]\nAuthFailureThreshold={threshold}\n'))
        with running_daemon(daemon, directory, (port, peer), args=DAEMON_ARGS, env=env,
                            console='console') as (_, ec):
            ec.sock.close()

            # Unsupported clients must never consume the failure budget,
            # even when an old GUI keeps retrying from the same address.
            for _ in range(12):
                with socket.create_connection(('127.0.0.1', port), timeout=10) as sock:
                    sock.sendall(W.packet('EC_OP_AUTH_REQ', [W.tag(
                        'EC_TAG_PROTOCOL_VERSION',
                        struct.pack('!H', C['EC_CURRENT_PROTOCOL_VERSION']), 3)]))
                    op, tags = W.read(sock)
                    assert op == C['EC_OP_AUTH_FAIL'] and b'3.3.0' in tags[C['EC_TAG_STRING']]
                with socket.create_connection(('127.0.0.1', port), timeout=10) as sock:
                    sock.sendall(W.packet('EC_OP_AUTH_REQ', [
                        W.tag('EC_TAG_SRP_A', W.pad(5)),
                        W.tag('EC_TAG_PROTOCOL_VERSION', b'\x00\x00', 3)]))
                    op, tags = W.read(sock)
                    assert op == C['EC_OP_AUTH_FAIL'] and b'Too many' not in tags[C['EC_TAG_STRING']]
            EC(port).sock.close()
            print('PASS: legacy and incompatible clients do not lock out SRP logins; limit', threshold)

            def abandon(tags):
                raise StopChallenge()

            def abandon_once():
                with socket.create_connection(('127.0.0.1', port), timeout=10) as sock:
                    try:
                        W.authenticate(sock, hashlib.md5(b'regression').digest(),
                                       challenge=abandon)
                    except StopChallenge:
                        return
                    raise AssertionError('challenge refused before the attempt limit')

            # Both production transports must complete the last admitted
            # attempt, and success must clear the reserved failures.
            for binary in (cmd, async_probe):
                for _ in range(threshold - 1):
                    abandon_once()
                args = ([cmd, '-h', '127.0.0.1', '-p', str(port), '-P',
                         'regression', '-c', 'status'] if binary == cmd else
                        [async_probe, str(port)])
                out = subprocess.run(args, env=env, capture_output=True, text=True, timeout=20)
                assert 'Succeeded!' in out.stdout + out.stderr, out

            # Allowing the reserved proof must not admit another challenge.
            for _ in range(threshold):
                abandon_once()
            with socket.create_connection(('127.0.0.1', port), timeout=10) as sock:
                _, op, tags = W.authenticate(sock, hashlib.md5(b'regression').digest())
                assert op == C['EC_OP_AUTH_FAIL'] and b'Too many' in tags[C['EC_TAG_STRING']]
            print('PASS: final reserved attempt succeeds and abandoned challenges lock out; limit', threshold)

def password_edit_regressions(port, root, peer):
    """Rejected edits must preserve the credential and unrelated preferences."""
    editor = EC(port)
    try:
        full = tag(C['EC_TAG_DETAIL_LEVEL'], bytes([C['EC_DETAIL_FULL']]), 2)
        select = tag(C['EC_TAG_SELECT_PREFS'], struct.pack('!I', C['EC_PREFS_GENERAL']), 4)
        def nickname():
            _, prefs = editor.call(C['EC_OP_GET_PREFERENCES'], [full, select])
            return prefs[C['EC_TAG_PREFS_GENERAL']][1][C['EC_TAG_USER_NICK']][0]
        original_nick = nickname()
        original_record = next(line for line in (root / 'amule.conf').read_text().splitlines()
                               if line.startswith('ECPasswordVerifier='))
        for value, kind in ((b'not-a-digest\0', 6), (bytes(16), 9),
                            (hashlib.md5(b'').digest(), 9)):
            remote = tag(C['EC_TAG_PREFS_REMOTECTRL'], children=[
                tag(C['EC_TAG_EXTERNALCONN_PASSWD'], value, kind)])
            general = tag(C['EC_TAG_PREFS_GENERAL'], children=[
                tag(C['EC_TAG_USER_NICK'], b'must-not-apply\0', 6)])
            assert editor.call(C['EC_OP_SET_PREFERENCES'], [full, general, remote])[0] == C['EC_OP_FAILED']
            assert nickname() == original_nick, 'rejected password edit changed other preferences'
            assert original_record in (root / 'amule.conf').read_text()
            EC(port).sock.close()

        # Never scan one remote group for authorization and apply another.
        empty_remote = tag(C['EC_TAG_PREFS_REMOTECTRL'], children=[])
        remote = tag(C['EC_TAG_PREFS_REMOTECTRL'], children=[
            tag(C['EC_TAG_EXTERNALCONN_PASSWD'], hashlib.md5(b'new-password').digest(), 9)])
        for groups in ([empty_remote, remote], [remote, empty_remote]):
            reply, _ = editor.call(C['EC_OP_SET_PREFERENCES'], [full] + groups)
            assert reply == C['EC_OP_FAILED'], 'duplicate remote-control groups accepted'
            assert original_record in (root / 'amule.conf').read_text()

        # A refused directory is a partial preference apply. It must never commit
        # a password that the client will treat as rejected and keep using the old one.
        directories = tag(C['EC_TAG_PREFS_DIRECTORIES'], children=[
            tag(C['EC_TAG_DIRECTORIES_INCOMING'],
                str(root.parent / (root.name + '-not-allowed')).encode() + b'\0', 6)])
        refused_password = tag(C['EC_TAG_PREFS_REMOTECTRL'], children=[
            tag(C['EC_TAG_EXTERNALCONN_PASSWD'], hashlib.md5(b'must-not-rotate').digest(), 9)])
        reply, error = editor.call(C['EC_OP_SET_PREFERENCES'], [full, directories, refused_password])
        assert reply == C['EC_OP_FAILED']
        assert b'The EC password was not changed.' in error[C['EC_TAG_STRING']][0]
        assert b'outside the allowed remote directories' in error[C['EC_TAG_STRING']][0]
        assert C['EC_TAG_SHAREDDIR_REJECTED'] in error
        assert original_record in (root / 'amule.conf').read_text(), 'refused folder committed password rotation'
        EC(port).sock.close()

        # Keep the port occupied until the daemon replies, guaranteeing bind failure.
        with socket.socket() as occupied:
            occupied.bind(('127.0.0.1', 0))
            occupied.listen()
            connection = tag(C['EC_TAG_PREFS_CONNECTIONS'], children=[
                tag(C['EC_TAG_CONN_TCP_PORT'], struct.pack('!H', occupied.getsockname()[1]), 3)])
            remote = tag(C['EC_TAG_PREFS_REMOTECTRL'], children=[
                tag(C['EC_TAG_EXTERNALCONN_PASSWD'], hashlib.md5(b'must-not-commit').digest(), 9)])
            for rejected in ([connection, remote], [connection, directories, remote]):
                reply, error = editor.call(C['EC_OP_SET_PREFERENCES'], [full] + rejected)
                assert reply == C['EC_OP_FAILED'] and b'The EC password was not changed.' in error[C['EC_TAG_STRING']][0]
                if directories in rejected:
                    assert b'outside the allowed remote directories' in error[C['EC_TAG_STRING']][0]
                    assert C['EC_TAG_SHAREDDIR_REJECTED'] in error
        assert original_record in (root / 'amule.conf').read_text(), 'failed port rebind committed a password rotation'
        EC(port).sock.close()
        with socket.create_connection(('127.0.0.1', peer), timeout=5):
            pass
    finally:
        editor.sock.close()
    print('PASS: malformed/empty password edits and failed port rebind preserve credentials and preferences')

def ec_config_survives_restart(daemon, cmd):
    with tempfile.TemporaryDirectory(prefix='amule-ec-config-') as scratch:
        root = Path(scratch)
        port, peer = free_ports(2)
        # A fresh install: external connections off and no password.
        write_amule_conf(root, port, Port=peer, UDPEnable=0,
                         sections={'ExternalConnect': {'ECAddress': '127.0.0.1', 'ECPort': port}})
        env = daemon_env(root)
        base = [daemon, '-c', scratch, *DAEMON_ARGS]
        for args in (['--ec-config'] + ([] if os.name == 'nt' else ['--enable-stdin']), []):
            with (root / 'console').open('w') as log:
                # Without a controlling terminal, getpass() reads stdin.
                proc = subprocess.Popen(base + args, env=env, stdout=log, stderr=log,
                                        stdin=subprocess.PIPE if args else subprocess.DEVNULL,
                                        start_new_session=True)
                try:
                    if args:
                        proc.stdin.write(b'regression\n')
                        proc.stdin.close()
                    connect_daemon(proc, port, root, peer).sock.close()
                    out = subprocess.run([cmd, '-h', '127.0.0.1', '-p', str(port), '-P', 'regression',
                                          '-c', 'status'],
                                         env=env, capture_output=True, text=True, timeout=20)
                    assert 'Succeeded!' in out.stdout + out.stderr, out
                finally:
                    stop_daemon(proc)
    print('PASS: amuled --ec-config keeps external connections on after a restart')

def main(daemon,cmd,async_probe):
    with tempfile.TemporaryDirectory(prefix='amule-srp-') as scratch:
        root=Path(scratch);port,peer=free_ports(2)
        write_amule_conf(root,port,Port=peer,UDPEnable=0)
        env=daemon_env(root)
        for restart in range(2):
            if restart:
                # Reproduce an unrelated settings save by a downgraded 3.1 core.
                path=root/'amule.conf'
                path.write_text(path.read_text().replace('[ExternalConnect]\n','[ExternalConnect]\nECPassword=\n'))
            with running_daemon(daemon, root, (port, peer), args=DAEMON_ARGS,
                                console='console') as (proc, ec):
                ec.sock.close()
                conf=(root/'amule.conf').read_text()
                assert '\nECPassword=' not in conf
                record=next(line for line in conf.splitlines() if line.startswith('ECPasswordVerifier='))
                assert record.startswith('ECPasswordVerifier=v1:15:8:1:')
                if restart: assert record==saved,'restart changed verifier/salt'
                saved=record
                out=subprocess.run([cmd,'-h','127.0.0.1','-p',str(port),'-P','regression','-c','status'],env=env,capture_output=True,text=True,timeout=20)
                assert 'Succeeded!' in out.stdout+out.stderr,out
                out=subprocess.run([async_probe,str(port)],env=env,capture_output=True,text=True,timeout=20)
                assert out.returncode==0,out
                print('PASS: migrated config, amulecmd and GUI async transport login, restart',restart)
                if restart:
                    # No M1 is sent. A bounded number of AUTH_REQ challenges
                    # must lock out this address before further SRP work.
                    challenges = 0
                    for attempt in range(12):
                        class StopChallenge(Exception): pass
                        try:
                            with socket.create_connection(('127.0.0.1',port),timeout=10) as sock:
                                def abandon(tags):
                                    raise StopChallenge()
                                _,op,tags=W.authenticate(sock,hashlib.md5(b'regression').digest(),challenge=abandon)
                                assert op==C['EC_OP_AUTH_FAIL'] and b'Too many' in tags[C['EC_TAG_STRING']]
                                break
                        except StopChallenge:
                            challenges += 1
                    else: raise AssertionError('abandoned handshakes bypass rate limit')
                    assert 0 < challenges <= 10
                    print('PASS: abandoned AUTH_REQ handshakes trigger pre-SRP rate limit')
                    continue
                # A legacy client gets an explicit version error before any password proof.
                with socket.create_connection(('127.0.0.1',port),timeout=10) as sock:
                    sock.sendall(W.packet('EC_OP_AUTH_REQ',[W.tag('EC_TAG_PROTOCOL_VERSION',struct.pack('!H',C['EC_CURRENT_PROTOCOL_VERSION']),3)]))
                    op,tags=W.read(sock)
                    assert op==C['EC_OP_AUTH_FAIL'] and b'3.3.0' in tags[C['EC_TAG_STRING']]
                # Importing daemon settings must preserve a remembered client credential.
                client_config=root/'config'/'remote.conf'
                client_config.parent.mkdir(exist_ok=True)
                remembered=hashlib.md5(b'regression').hexdigest()
                client_config.write_text('[EC]\nPassword='+remembered+'\n')
                out=subprocess.run([cmd,'--config-file=remote.conf','--create-config-from='+str(root/'amule.conf')],cwd=root,env=env,capture_output=True,text=True,timeout=10)
                assert out.returncode==0,out
                imported = read_config(client_config)
                assert imported['EC']['Password'].lower() == remembered, 'config import erased remembered credential'
                out = subprocess.run(
                    [cmd, '--config-file=remote.conf', '-c', 'status'],
                    cwd=root, env=env, capture_output=True, text=True, timeout=20)
                assert out.returncode == 0 and 'Succeeded!' in out.stdout + out.stderr, out
                # The retired setting cannot opt a connector out of encrypted SRP.
                for encryption in ('0', '1'):
                    client_config.write_text(
                        f'[EC]\nHost=127.0.0.1\nPort={port}\n'
                        f'Password={remembered}\nEncryption={encryption}\n')
                    out = subprocess.run(
                        [cmd, '--config-file=remote.conf', '-c', 'status'],
                        cwd=root, env=env, capture_output=True, text=True, timeout=20)
                    assert out.returncode == 0 and 'Succeeded!' in out.stdout + out.stderr, out
                print('PASS: remote.conf Encryption=0 and Encryption=1 are ignored on load')
                # Missing token follows a full SRP exchange and rejects a guess.
                with socket.create_connection(('127.0.0.1',port),timeout=10) as sock:
                    encrypted,op,_=W.authenticate(sock,os.urandom(16),credential=1)
                    assert op==C['EC_OP_AUTH_FAIL'] and isinstance(encrypted, W.EncryptedSocket)
                with FixtureServer(('127.0.0.1',0),Relay) as relay:
                    relay.target=port;relay.error=None;relay.observed=None
                    threading.Thread(target=relay.serve_forever,daemon=True).start()
                    with socket.create_connection(relay.server_address,timeout=15) as sock:
                        _,op,_=W.authenticate(sock,hashlib.md5(b'regression').digest())
                        assert op==C['EC_OP_AUTH_FAIL']
                    relay.shutdown()
                    assert relay.error is None,relay.error
                    assert len(relay.observed)==32
                print('PASS: old client, missing token, split X25519 MITM rejected')
                EC(port).sock.close()
                # Every invalid sequence consumes its connection, and replayed
                # M1 must fail even on a fresh transport with the same password.
                for opcode in ('EC_OP_AUTH_PASSWD', 'EC_OP_STAT_REQ'):
                    with socket.create_connection(('127.0.0.1',port),timeout=10) as sock:
                        sock.sendall(W.packet(opcode,[]))
                        assert W.read(sock)[0]==C['EC_OP_AUTH_FAIL']
                for value in (bytes(384), W.pad(W.N), W.pad(W.N+1), b'bad'):
                    with socket.create_connection(('127.0.0.1',port),timeout=10) as sock:
                        def mutate(fields):
                            return [f for f in fields if f[:2]!=W.tag('EC_TAG_SRP_A',b'')[:2]]+[W.tag('EC_TAG_SRP_A',value)]
                        assert W.authenticate(sock,hashlib.md5(b'regression').digest(),request_mutator=mutate)[1]==C['EC_OP_AUTH_FAIL']
                # Successful logins clear the attempt bucket between phases.
                EC(port).sock.close()
                with socket.create_connection(('127.0.0.1',port),timeout=10) as sock:
                    assert W.authenticate(sock,hashlib.md5(b'regression').digest(),proof_mutator=lambda m: W.packet('EC_OP_AUTH_REQ',[]))[1]==C['EC_OP_AUTH_FAIL']
                # Reject the whole buffered batch: a valid M1 behind a dropped
                # clear/invalid-flags packet must not register a dying notifier.
                clear = W.packet('EC_OP_STAT_REQ', [])
                for prefix in (clear, struct.pack('!I', 0x60) + clear[4:]):
                    granted = (root / 'logfile').read_text().count('Access granted.')
                    with socket.create_connection(('127.0.0.1', port), timeout=10) as sock:
                        try:
                            W.authenticate(sock, hashlib.md5(b'regression').digest(),
                                capabilities=[W.tag('EC_TAG_CAN_NOTIFY')], proof_prefix=prefix)
                        except (EOFError, ConnectionResetError, BrokenPipeError):
                            pass
                        else:
                            raise AssertionError('proof behind rejected packet was dispatched')
                    time.sleep(1)
                    assert proc.poll() is None, 'rejected batch killed the daemon'
                    assert (root / 'logfile').read_text().count('Access granted.') == granted, 'buffered proof authorized a dropped socket'
                    EC(port).sock.close()
                saved_m1=[]
                with socket.create_connection(('127.0.0.1',port),timeout=10) as sock:
                    def capture(m):
                        saved_m1.append(m)
                        return W.packet('EC_OP_AUTH_PASSWD',[W.tag('EC_TAG_SRP_M1',m)])
                    assert W.authenticate(sock,hashlib.md5(b'regression').digest(),proof_mutator=capture)[1]==C['EC_OP_AUTH_OK']
                with socket.create_connection(('127.0.0.1',port),timeout=10) as sock:
                    assert W.authenticate(sock,hashlib.md5(b'regression').digest(),proof_mutator=lambda m: W.packet('EC_OP_AUTH_PASSWD',[W.tag('EC_TAG_SRP_M1',saved_m1[0])]))[1]==C['EC_OP_AUTH_FAIL']
                EC(port).sock.close()
                print('PASS: out-of-order, repeated AUTH_REQ, invalid A, and M1 replay rejected')

                password_edit_regressions(port, root, peer)
                def change_password(old, new):
                    editor=EC(port,password=old)
                    try:
                        request=tag(C['EC_TAG_PREFS_REMOTECTRL'],children=[
                            tag(C['EC_TAG_EXTERNALCONN_PASSWD'],hashlib.md5(new.encode()).digest(),9)])
                        op,_=editor.call(C['EC_OP_SET_PREFERENCES'],[request])
                        assert op==C['EC_OP_NOOP']
                    finally: editor.sock.close()
                with socket.create_connection(('127.0.0.1',port),timeout=15) as sock:
                    _,op,_=W.authenticate(sock,hashlib.md5(b'regression').digest(),
                        before_proof=lambda: change_password('regression','rotated'))
                    assert op==C['EC_OP_AUTH_FAIL'],'in-flight old password accepted after rotation'
                for args in ([cmd, '-h', '127.0.0.1', '-p', str(port), '-P', 'rotated', '-c', 'status'],
                             [async_probe, str(port), 'rotated']):
                    out = subprocess.run(args, env=env, capture_output=True, text=True, timeout=20)
                    assert 'Succeeded!' in out.stdout + out.stderr, out
                rotated = next(line for line in (root/'amule.conf').read_text().splitlines()
                               if line.startswith('ECPasswordVerifier='))
                change_password('rotated', 'rotated')
                again = next(line for line in (root/'amule.conf').read_text().splitlines()
                             if line.startswith('ECPasswordVerifier='))
                assert again != rotated, 'explicit password update reused the previous salt/verifier'
                change_password('rotated','regression')
                saved=next(line for line in (root/'amule.conf').read_text().splitlines()
                           if line.startswith('ECPasswordVerifier='))
                print('PASS: remote password change invalidates pending old-password proof')

        rate_limit_boundary(daemon, cmd, async_probe, root, env)
        with FixtureServer(('127.0.0.1',0),OldDaemon) as old:
            old.extra=None
            threading.Thread(target=old.serve_forever,daemon=True).start()
            out=subprocess.run([cmd,'-h','127.0.0.1','-p',str(old.server_address[1]),'-P','regression','-c','status'],env=env,capture_output=True,text=True,timeout=10)
            assert '3.3.0 or later' in out.stdout+out.stderr,out
            old.shutdown()
            assert old.extra==b'',old.extra
        with FixtureServer(('127.0.0.1',0),OldDaemon) as malicious:
            malicious.bad_profile=True;malicious.extra=None
            threading.Thread(target=malicious.serve_forever,daemon=True).start()
            start=time.monotonic()
            out=subprocess.run([cmd,'-h','127.0.0.1','-p',str(malicious.server_address[1]),'-P','regression','-c','status'],env=env,capture_output=True,text=True,timeout=5)
            assert 'unsupported profile' in out.stdout+out.stderr,out
            assert time.monotonic()-start<3,'client attempted unbounded KDF'
            malicious.shutdown();assert malicious.extra==b''
        print('PASS: unauthenticated excessive scrypt cost rejected without proof')
        for binary in (cmd, async_probe):
            for missing, plaintext in ((False, False), (True, False), (False, True)):
                with FixtureServer(('127.0.0.1', 0), BadServerProof) as bad:
                    bad.missing = missing
                    bad.plaintext = plaintext
                    bad.extra = None
                    bad.error = None
                    threading.Thread(target=bad.serve_forever, daemon=True).start()
                    args = ([cmd, '-h', '127.0.0.1', '-p', str(bad.server_address[1]),
                             '-P', 'regression', '-c', 'status'] if binary == cmd else
                            [async_probe, str(bad.server_address[1])])
                    start = time.monotonic()
                    out = subprocess.run(args, env=env, capture_output=True, text=True,
                                         timeout=10 if plaintext else 20)
                    message = out.stdout + out.stderr
                    assert ('failed to prove' in message or
                            (plaintext and 'unencrypted packet' in message)), out
                    if plaintext:
                        assert time.monotonic() - start < 10, 'authentication failure waited for watchdog'
                    assert 'Succeeded!' not in out.stdout + out.stderr, out
                    bad.shutdown()
                    assert bad.error is None, bad.error
                    assert bad.extra == b'', 'command sent before validating daemon proof'
        print('PASS: amulecmd and GUI transport reject missing/tampered server proofs before commands')
        with FixtureServer(('127.0.0.1', 0), PlaintextReply) as bad:
            bad.error = None
            threading.Thread(target=bad.serve_forever, daemon=True).start()
            out = subprocess.run([cmd, '-h', '127.0.0.1', '-p', str(bad.server_address[1]),
                                  '-P', 'regression', '-c', 'status'],
                                 env=env, capture_output=True, text=True, timeout=5)
            bad.shutdown()
            assert bad.error is None, bad.error
            assert 'injected plaintext response' not in out.stdout + out.stderr, out
            assert 'unencrypted packet' in out.stdout + out.stderr, out
        print('PASS: synchronous command replies require encryption after authentication')
        out=subprocess.run([cmd,'--disable-ec-encryption'],env=env,capture_output=True,text=True,timeout=5)
        assert 'Unknown' in out.stdout+out.stderr or 'unknown' in out.stdout+out.stderr,out
        print('PASS: old daemon and removed encryption switch fail clearly')
    ec_config_survives_restart(daemon, cmd)

if __name__=='__main__': main(*sys.argv[1:])
