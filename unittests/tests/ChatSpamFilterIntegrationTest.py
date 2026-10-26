#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Test daemon chat protection with offline peers and an isolated configuration.

Debug builds additionally use the existing CAPTCHA diagnostic to supply a correct
answer, without teaching this fixture to recognize the generated image.
"""
from contextlib import contextmanager
import hashlib
import itertools
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import tempfile

from AllSearchIntegrationTest import (C, connect_daemon, exact, free_ports, integer,
                                      retry_port_collisions, stop_daemon, string, tag)
from DaemonConfig import write_amule_conf


def frame(peer, protocol, opcode, data=b''):
    peer.sendall(struct.pack('<BIB', protocol, len(data) + 1, opcode) + data)


def packet(peer):
    protocol, length = struct.unpack('<BI', exact(peer, 5))
    assert 1 <= length < 65536
    data = exact(peer, length)
    return protocol, data[0], data[1:]


peer_addresses = itertools.count(2)


@contextmanager
def sender(port, name, captcha=True):
    address = f'127.0.0.{next(peer_addresses)}'
    with socket.socket() as identity, socket.socket() as peer:
        identity.bind((address, 0))
        peer.bind((address, 0))
        peer.settimeout(5)
        peer.connect(('127.0.0.1', port))
        name = name.encode()
        peer_hash = hashlib.md5(name).digest()
        tags = (b'\x02\x01\x00\x01' + struct.pack('<H', len(name)) + name +
                b'\x03\x01\x00\xfe' + struct.pack('<I', (1 << 11) if captcha else 0))
        hello = (b'\x10' + peer_hash + socket.inet_aton(address) +
                 struct.pack('<HI', identity.getsockname()[1], 2) + tags + bytes(6))
        frame(peer, 0xe3, 1, hello)
        while packet(peer)[:2] != (0xe3, 0x4c):
            pass

        def message(text):
            encoded = text.encode()
            frame(peer, 0xe3, 0x4e, struct.pack('<H', len(encoded)) + encoded)
            # A HELLOANSWER is a barrier for all preceding frames on this socket.
            frame(peer, 0xe3, 1, hello)
            received = []
            while True:
                reply = packet(peer)
                if reply[:2] == (0xe3, 0x4c):
                    return received
                received.append(reply)
        yield peer_hash, message


def preferences(ec):
    op, tags = ec.call(C['EC_OP_GET_PREFERENCES'],
                      [integer(C['EC_TAG_SELECT_PREFS'], C['EC_PREFS_MESSAGEFILTER'])])
    assert op == C['EC_OP_SET_PREFERENCES'], (op, tags)
    return tags[C['EC_TAG_PREFS_MESSAGEFILTER']][1]


def settings(ec, advanced, captcha):
    group = tag(C['EC_TAG_PREFS_MESSAGEFILTER'], children=[
        integer(C['EC_TAG_MSGFILTER_ADVANCED'], advanced),
        integer(C['EC_TAG_MSGFILTER_CAPTCHA'], captcha)])
    # Explicit booleans work at CMD as well as FULL. At CMD old presence
    # booleans in the same group are cleared, including the plain filter.
    assert ec.call(C['EC_OP_SET_PREFERENCES'], [group])[0] == C['EC_OP_NOOP']
    read = preferences(ec)
    assert read[C['EC_TAG_MSGFILTER_ADVANCED']][0] == advanced
    assert read[C['EC_TAG_MSGFILTER_CAPTCHA']][0] == captcha


def latest(ec, peer_hash):
    op, tags = ec.call(C['EC_OP_GET_CHAT_MESSAGES'],
                      [tag(C['EC_TAG_CHAT_PEER_HASH'], peer_hash, 9)])
    if op == C['EC_OP_FAILED']:
        return None
    assert op == C['EC_OP_CHAT_MESSAGES'], (op, tags)
    fields = tags[C['EC_TAG_CHAT_SESSION']][1]
    return fields[C['EC_TAG_CHAT_MESSAGE']][0].rstrip(b'\0').decode()


def close(ec, peer_hash):
    assert ec.call(C['EC_OP_CHAT_CLOSE_SESSION'],
                   [tag(C['EC_TAG_CHAT_PEER_HASH'], peer_hash, 9)])[0] == C['EC_OP_NOOP']
    assert latest(ec, peer_hash) is None


def challenge(replies):
    images = [data for proto, op, data in replies if (proto, op) == (0xc5, 0xa5)]
    assert len(images) == 1, replies
    data = images[0]
    assert data[0] == 0 and data[1:3] == b'BM'
    assert len(data) == 639, len(data)
    assert struct.unpack_from('<I', data, 3)[0] == len(data) - 1
    assert struct.unpack_from('<iiHHI', data, 19) == (96, 48, 1, 1, 0)


def run(binary, debug):
    with tempfile.TemporaryDirectory(prefix='amule-chat-spam-') as directory:
        root = Path(directory)
        ports = free_ports(3, udp_indices=(2,), peer_index=1)
        write_amule_conf(root, ports[0], Port=ports[1], UDPPort=ports[2], UDPEnable=0,
                         FilterLanIPs=0, FilterMessages=0, VerboseDebug=1,
                         VerboseDebugLogfile=0, sections={'Debug': {'Cat_ED2k Client': 1}})
        env = dict(os.environ, HOME=str(root), XDG_CONFIG_HOME=str(root / 'xdg'), LC_ALL='C.UTF-8')
        env.pop('LD_PRELOAD', None)
        env.pop('DISPLAY', None)
        with (root / 'console.log').open('w') as log:
            proc = subprocess.Popen([binary, '-c', str(root), '--disable-fatal'],
                                    stdout=log, stderr=log, env=env)
        ec = None
        try:
            ec = connect_daemon(proc, ports[0], root, ports[1], None)
            prefs = preferences(ec)
            assert prefs[C['EC_TAG_MSGFILTER_ADVANCED']][0] == 1
            assert prefs[C['EC_TAG_MSGFILTER_CAPTCHA']][0] == 1
            with sender(ports[1], 'default-on') as (peer_hash, message):
                challenge(message('hello'))
                assert latest(ec, peer_hash) is None

            # An old GUI saves this whole group without the two new tags.
            for detail in (C['EC_DETAIL_CMD'], C['EC_DETAIL_FULL']):
                old_group = tag(C['EC_TAG_PREFS_MESSAGEFILTER'], children=[
                    integer(C['EC_TAG_MSGFILTER_SHOW_IN_LOG'], 1)])
                assert ec.call(C['EC_OP_SET_PREFERENCES'], [old_group,
                    integer(C['EC_TAG_DETAIL_LEVEL'], detail)])[0] == C['EC_OP_NOOP']
                prefs = preferences(ec)
                assert prefs[C['EC_TAG_MSGFILTER_ADVANCED']][0] == 1
                assert prefs[C['EC_TAG_MSGFILTER_CAPTCHA']][0] == 1

            with sender(ports[1], 'challenge-retries') as (peer_hash, message):
                for attempt in range(3):
                    challenge(message('withheld'))
                    assert latest(ec, peer_hash) is None
                    replies = message('wrong')
                    assert (0xc5, 0xa6, bytes([1 if attempt < 2 else 2])) in replies, replies
                    assert latest(ec, peer_hash) is None
                assert not message('exhausted')
                assert latest(ec, peer_hash) is None

            if debug:
                with sender(ports[1], 'challenge-success') as (peer_hash, message):
                    text = 'hello after solving'
                    challenge(message(text))
                    # This diagnostic exists only in Debug builds. The production
                    # RNG and network handler still generate and verify the answer.
                    logged = (root / 'logfile').read_text()
                    answers = re.findall(r'sent Captcha ([A-Z1-9]{4}) \(', logged)
                    assert answers, logged[-3000:]
                    # Exercise the real answer path with visual substitutions; the unit
                    # test covers every pair deterministically.
                    answer = answers[-1].translate(str.maketrans('SZGB', '5268'))
                    replies = message('  ' + answer.lower() + '  ')
                    assert (0xc5, 0xa6, b'\0') in replies, replies
                    assert latest(ec, peer_hash) == text
                    assert not message('another greeting')
                    assert latest(ec, peer_hash) == 'another greeting'
                    # Explicit spam marks still apply after a successful challenge.
                    filtered = tag(C['EC_TAG_PREFS_MESSAGEFILTER'], children=[
                        integer(C['EC_TAG_MSGFILTER_ENABLED'], 1),
                        integer(C['EC_TAG_MSGFILTER_BY_KEYWORD'], 1),
                        string(C['EC_TAG_MSGFILTER_KEYWORDS'], 'forbidden')])
                    assert ec.call(C['EC_OP_SET_PREFERENCES'], [filtered])[0] == C['EC_OP_NOOP']
                    assert not message('forbidden')
                    assert latest(ec, peer_hash) == 'another greeting'
                    assert not message('benign after being marked as spam')
                    assert latest(ec, peer_hash) is None
                settings(ec, 1, 1)
                for scenario in ('pending-url', 'later-url', 'unanswered'):
                    with sender(ports[1], scenario) as (peer_hash, message):
                        text = 'http://example.com' if scenario == 'pending-url' else 'hello'
                        challenge(message(text))
                        answers = re.findall(r'sent Captcha ([A-Z1-9]{4}) \(',
                                             (root / 'logfile').read_text())
                        assert answers
                        assert (0xc5, 0xa6, b'\0') in message(answers[-1])
                        if scenario == 'pending-url':
                            assert latest(ec, peer_hash) is None
                        else:
                            assert latest(ec, peer_hash) == text
                            if scenario == 'later-url':
                                assert not message('http://example.com')
                            else:
                                for n in range(3):
                                    assert not message(f'unanswered {n}')
                                    assert latest(ec, peer_hash) == f'unanswered {n}'
                                assert not message('fifth message')
                            assert latest(ec, peer_hash) is None

            with sender(ports[1], 'unsupported', captcha=False) as (peer_hash, message):
                replies = message('hello')
                notices = [data for proto, op, data in replies if (proto, op) == (0xe3, 0x4e)]
                assert len(notices) == 1 and b'captcha' in notices[0], replies
                assert not message('again')
                assert latest(ec, peer_hash) is None

            # The two switches remain independent: disabling the parent bypasses
            # CAPTCHA even while its stored sub-option is on.
            settings(ec, 0, 1)
            with sender(ports[1], 'disabled-parent') as (peer_hash, message):
                assert not message('http://example.com accepted')
                assert latest(ec, peer_hash) == 'http://example.com accepted'
                close(ec, peer_hash)

            settings(ec, 1, 0)
            with sender(ports[1], 'url-spam') as (peer_hash, message):
                assert not message('hello')
                assert latest(ec, peer_hash) == 'hello'
                assert not message('http://example.com unsolicited')
                assert latest(ec, peer_hash) is None
            with sender(ports[1], 'unanswered-spam') as (peer_hash, message):
                for i in range(4):
                    assert not message(f'unanswered {i}')
                    assert latest(ec, peer_hash) == f'unanswered {i}'
                assert not message('fifth message')
                assert latest(ec, peer_hash) is None

            settings(ec, 1, 1)
            friend_name = 'friend-bypass'
            friend_hash = hashlib.md5(friend_name.encode()).digest()
            friend = tag(C['EC_TAG_FRIEND_ADD'], children=[
                tag(C['EC_TAG_FRIEND_HASH'], friend_hash, 9),
                integer(C['EC_TAG_FRIEND_IP'], 0x0100007f),
                integer(C['EC_TAG_FRIEND_PORT'], 4662),
                string(C['EC_TAG_FRIEND_NAME'], friend_name)])
            assert ec.call(C['EC_OP_FRIEND'], [friend])[0] == C['EC_OP_NOOP']
            with sender(ports[1], friend_name, captcha=False) as (peer_hash, message):
                assert not message('http://example.com friend')
                assert latest(ec, peer_hash) == 'http://example.com friend'
                close(ec, peer_hash)

            # Once we write first, an unsolicited-message challenge must not fire.
            settings(ec, 1, 1)
            with sender(ports[1], 'user-initiated') as (peer_hash, message):
                assert ec.call(C['EC_OP_CHAT_SEND'], [tag(C['EC_TAG_CHAT_PEER_HASH'], peer_hash, 9),
                    string(C['EC_TAG_CHAT'], 'our greeting')])[0] == C['EC_OP_NOOP']
                replies = message('http://example.com reply')
                assert not any((proto, op) == (0xc5, 0xa5) for proto, op, _ in replies)
                assert latest(ec, peer_hash) == 'http://example.com reply'
                close(ec, peer_hash)

            # Explicit opt-out values survive restart.
            settings(ec, 0, 0)
            ec.sock.close()
            ec = None
            stop_daemon(proc)
            with (root / 'console.log').open('a') as log:
                proc = subprocess.Popen([binary, '-c', str(root), '--disable-fatal'],
                                        stdout=log, stderr=log, env=env)
            ec = connect_daemon(proc, ports[0])
            prefs = preferences(ec)
            assert prefs[C['EC_TAG_MSGFILTER_ADVANCED']][0] == 0
            assert prefs[C['EC_TAG_MSGFILTER_CAPTCHA']][0] == 0
            print('PASS: daemon defaults, explicit EC preferences, old clients, challenge/retry/exhaustion, '
                  'unsupported peers, parent toggle, URL/unanswered spam, initiated chats, persistence'
                  + (', correct solution and post-solve spam filtering' if debug else ''))
        except BaseException:
            print((root / 'console.log').read_text()[-6000:])
            raise
        finally:
            if ec is not None:
                ec.sock.close()
            if proc.poll() is None:
                stop_daemon(proc)


if __name__ == '__main__':
    retry_port_collisions(run, sys.argv[1], '--debug-build' in sys.argv[2:])
