#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Hold a drive's reads/opens while other drives continue uploading (Linux).

The preload fixture substitutes a mount-table snapshot with two storage IDs.
Two distinct directories have the same ID, modelling bind/volume aliases. Four
clients target that drive: an unrestricted four-thread pool would stall every
worker. All filesystem delays are pipe-controlled; no real mounts are changed.
"""
import argparse
import os
from pathlib import Path
import select
import struct
import subprocess
import tempfile
import time
import zlib

from AllSearchIntegrationTest import connect_daemon, free_ports, retry_port_collisions, stop_daemon
from DaemonConfig import write_amule_conf
from ShareWatcherMoveIntegrationTest import shared
from UploadStorageDelayIntegrationTest import BLOCK, Peer, wait_read
from UploadRefillIntegrationTest import packet, send


def run(binary, fixture, stage):
    if stage in ('mapped-copy', 'mapped-fault') and os.getenv('AMULE_TEST_SKIP_MMAP') == '1':
        print(f'{stage}: skipped because mmap support is disabled in this build')
        return
    with tempfile.TemporaryDirectory(prefix='amule-storage-pool-') as directory:
        root = Path(directory)
        incoming, slow, alias = root / 'Incoming', root / 'Slow', root / 'SlowAlias'
        for path in (incoming, slow, alias, root / 'Temp'):
            path.mkdir()
        bad_data = [os.urandom((6 if stage == 'fairness' else 1) * BLOCK) for _ in range(4)]
        if stage in ('packet-session', 'heap-buffer'):
            bad_data[0] = os.urandom(4096) * (2 * BLOCK // 4096)
        slow_name = 'bad-0.txt' if stage in ('packet-session', 'heap-buffer') else 'bad-0.bin'
        for index, data in enumerate(bad_data):
            ((slow if index < 2 else alias) / (slow_name if index == 0 else f'bad-{index}.bin')).write_bytes(data)
        healthy_data = os.urandom((7 if stage == 'fairness' else 3) * BLOCK)
        (incoming / 'healthy.bin').write_bytes(healthy_data)
        (root / 'shareddir.dat').write_text(f'{slow}\n{alias}\n')
        # Escape paths as mountinfo does, including user TMPDIR paths containing spaces.
        def escaped(path):
            return str(path).replace('\\', '\\134').replace(' ', '\\040').replace('\t', '\\011').replace('\n', '\\012')
        mounts = root / 'mountinfo'
        mounts.write_text('1 0 0:1 / / rw - tmpfs root rw\n'
                          + f'2 1 8:1 / {escaped(slow)} rw - ext4 synthetic-a rw\n'
                          + f'3 1 8:1 /alias {escaped(alias)} rw - ext4 synthetic-a rw\n'
                          + f'4 1 8:2 / {escaped(incoming)} rw - ext4 synthetic-b rw\n')
        ec_port, peer_port = free_ports(2, peer_index=1)
        write_amule_conf(root, ec_port, Port=peer_port, UDPEnable=0, FilterLanIPs=0,
                         MaxUpload=3500, SlotAllocation=100, MMapEnabled=int(stage in ('mapped-copy', 'mapped-fault')),
                         SharedFileReverifyDays=0, IncomingDir=incoming)
        notify_r, notify_w = os.pipe()
        release_r, release_w = os.pipe()
        arm = root / 'armed'
        env = dict(os.environ, HOME=str(root), XDG_CONFIG_HOME=str(root / 'xdg'), LC_ALL='C.UTF-8',
                   LD_PRELOAD=os.pathsep.join(filter(None, (os.getenv('AMULE_TEST_PRELOAD_RUNTIME'), fixture))), AMULE_TEST_READ_PREFIX=str(root / 'Slow'),
                   AMULE_TEST_READ_STAGE='open' if stage == 'open' else 'read', AMULE_TEST_MOUNTINFO=str(mounts),
                   AMULE_TEST_READ_ARM=str(arm), AMULE_TEST_READ_NOTIFY_FD=str(notify_w),
                   AMULE_TEST_READ_RELEASE_FD=str(release_r))
        if stage == 'discovery':
            env['AMULE_TEST_READ_STAGE'] = 'discovery'
            env['AMULE_TEST_READ_PATH'] = '/proc/self/mountinfo'
        if stage == 'packet-session':
            env['AMULE_TEST_READ_STAGE'] = 'pack'
            env['AMULE_TEST_READ_PATH'] = 'upload-packet-build'
        if stage in ('mapped-copy', 'mapped-fault', 'heap-buffer'):
            env['AMULE_TEST_READ_STAGE'] = 'heap' if stage == 'heap-buffer' else 'copy'
            env['AMULE_TEST_READ_PATH'] = str(slow / slow_name)
        peers = []
        ec = proc = None
        with (root / 'console.log').open('w') as log:
            try:
                proc = subprocess.Popen([binary, '-c', str(root), '--disable-fatal'], env=env,
                                        stdout=log, stderr=log, pass_fds=(notify_w, release_r))
                os.close(notify_w)
                notify_w = None
                os.close(release_r)
                release_r = None
                ec = connect_daemon(proc, ec_port, root, peer_port)
                deadline = time.monotonic() + 30
                while True:
                    files = shared(ec)
                    if len(files) == 5:
                        break
                    assert time.monotonic() < deadline, f'shares missing: {files}'
                    time.sleep(0.05)
                bad_count = 1 if stage in ('session', 'packet-session', 'heap-buffer', 'mapped-copy', 'mapped-fault', 'shutdown', 'discovery', 'open-failure') else (2 if stage == 'fairness' else 4)
                for index in range(bad_count):
                    peers.append(Peer(peer_port, index,
                                      files[slow_name if index == 0 else f'bad-{index}.bin'],
                                      compress=stage in ('packet-session', 'heap-buffer')))
                peers.append(Peer(peer_port, bad_count, files['healthy.bin']))
                healthy = peers[bad_count]
                probe = (6 * BLOCK, 7 * BLOCK) if stage == 'fairness' else (BLOCK, 2 * BLOCK)
                healthy.request([probe])
                healthy.receive(probe, healthy_data)
                arm.touch()
                if stage == 'discovery':
                    wait_read(notify_r, (1 << 64) - 2)
                    # A provider stuck resolving mounts/mapped drives must not stop
                    # the coordinator or replace the last successful snapshot.
                    healthy.request([(0, BLOCK)])
                    healthy.receive((0, BLOCK), healthy_data)
                    peers[0].request([(0, BLOCK)])
                    peers[0].receive((0, BLOCK), bad_data[0])
                    os.write(release_w, b'R')
                    print('discovery: uploads continued while storage discovery was held')
                    return
                if stage in ('mapped-copy', 'mapped-fault'):
                    busy = peers[0]
                    busy.request([(0, BLOCK)])
                    wait_read(notify_r, 0)  # mapped pages have been prefaulted; copy has not run
                    if stage == 'mapped-fault':
                        # Truncation invalidates even resident mapped pages. The copy will
                        # SIGBUS, and recovery must reject its zero-filled replacement.
                        (slow / slow_name).write_bytes(b'')
                    os.write(release_w, b'R')
                    healthy.request([(0, BLOCK)])
                    healthy.receive((0, BLOCK), healthy_data)
                    if stage == 'mapped-copy':
                        busy.receive((0, BLOCK), bad_data[0])
                    else:
                        # Check pending payload while asking the main thread for a
                        # protocol barrier; a failed block must never send recovered zeros.
                        send(busy.sock, 0x58, busy.file_hash)
                        while True:
                            proto, opcode, data = packet(busy.sock)
                            assert (proto, opcode) not in ((0xe3, 0x46), (0xc5, 0x40)), 'faulted block was sent'
                            if (proto, opcode) == (0xe3, 0x59):
                                assert data[:16] == busy.file_hash
                                break
                    assert proc.poll() is None, 'daemon exited after a mapped-copy fault'
                    print(f'{stage}: mapped upload copy checked; healthy peer continued, payload integrity verified')
                    return
                if stage == 'heap-buffer':
                    busy = peers[0]
                    busy.request([(0, BLOCK)])
                    wait_read(notify_r, BLOCK)  # fixture verified the compressor sees the heap read buffer
                    os.write(release_w, b'R')
                    proto, opcode, data = packet(busy.sock)
                    assert (proto, opcode) == (0xc5, 0x40)
                    assert data[:16] == busy.file_hash
                    start, packed_size = struct.unpack_from('<II', data, 16)
                    assert start == 0 and len(data[24:]) == packed_size
                    assert zlib.decompress(data[24:]) == bad_data[0][:BLOCK]
                    print('heap-buffer: compressor reused the original heap read buffer; payload verified')
                    return
                if stage == 'packet-session':
                    busy = peers[0]
                    busy.request([(0, BLOCK)])
                    wait_read(notify_r, BLOCK)  # first block is being compressed
                    # The main thread must be able to reuse the same client/socket
                    # while the coordinator builds the old session's packets.
                    send(busy.sock, 0x56, b'')
                    busy.barrier()
                    send(busy.sock, 0x54, busy.file_hash)
                    busy.until(0x55)
                    busy.request([(BLOCK, 2 * BLOCK)])
                    busy.barrier()
                    os.write(release_w, b'R')
                    wait_read(notify_r, BLOCK)  # new session's block is now being packed
                    os.write(release_w, b'R')
                    proto, opcode, data = packet(busy.sock)
                    assert (proto, opcode) == (0xc5, 0x40), 'expected a compressed upload block'
                    assert data[:16] == busy.file_hash, 'wrong upload file'
                    start, packed_size = struct.unpack_from('<II', data, 16)
                    assert start == BLOCK, 'old session block sent after packet construction'
                    packed = data[24:]
                    while len(packed) < packed_size:
                        proto, opcode, data = packet(busy.sock)
                        assert (proto, opcode) == (0xc5, 0x40), 'expected compressed continuation'
                        assert data[:24] == busy.file_hash + struct.pack('<II', start, packed_size)
                        packed += data[24:]
                    assert len(packed) == packed_size, 'compressed length differs'
                    assert zlib.decompress(packed) == bad_data[0][BLOCK:2 * BLOCK], 'upload data differs'
                    print('packet-session: slot reused while packing; old packets discarded, new payload verified')
                    return
                if stage == 'open-failure':
                    # The request owns an uncached entry, but Open() never produces a handle.
                    # Worker cleanup must not try to close that unopened CFile.
                    (slow / slow_name).unlink()
                    peers[0].request([(0, BLOCK)])
                    deadline = time.monotonic() + 10
                    while slow_name in shared(ec):
                        assert proc.poll() is None and time.monotonic() < deadline, \
                            'failed open was not completed'
                    healthy.request([(0, BLOCK)])
                    healthy.receive((0, BLOCK), healthy_data)
                    assert proc.poll() is None, 'daemon exited while cleaning up a failed open'
                    print('open-failure: unopened upload handle cleaned up; healthy peer continued')
                    return
                expected = (1 << 64) - 1 if stage == 'open' else 0
                peers[0].request([(0, BLOCK)])
                wait_read(notify_r, expected)
                if stage == 'fairness':
                    peers[0].request([(BLOCK, 2 * BLOCK), (2 * BLOCK, 3 * BLOCK)])
                    peers[1].request([(i * BLOCK, (i + 1) * BLOCK) for i in range(3, 6)])
                    for peer in peers[:2]:
                        peer.barrier()
                    # A fast peer finishes another read before every slow-volume
                    # release. A global rotation cursor then always picks peer 0.
                    order = [(0, 0), (1, 3), (0, 1), (1, 4), (0, 2), (1, 5)]
                    for index, (peer_index, block_index) in enumerate(order):
                        fast_start = index * BLOCK
                        healthy.request([(fast_start, fast_start + BLOCK)])
                        healthy.receive((fast_start, fast_start + BLOCK), healthy_data)
                        os.write(release_w, b'R')
                        if index + 1 < len(order):
                            wait_read(notify_r, order[index + 1][1] * BLOCK)
                        peers[peer_index].receive((block_index * BLOCK, (block_index + 1) * BLOCK),
                                                  bad_data[peer_index])
                    print('fairness: both slow-volume peers alternated despite fast-volume dispatches; payloads verified')
                    return
                if stage == 'session':
                    # Keep the socket/client identity and ask for the same block in a
                    # new upload session. The old read must not satisfy its request.
                    busy = peers[0]
                    send(busy.sock, 0x56, b'')
                    busy.barrier()
                    send(busy.sock, 0x54, busy.file_hash)
                    busy.until(0x55)
                    busy.request([(0, BLOCK)])
                    busy.barrier()
                    healthy.request([(0, BLOCK)])
                    healthy.receive((0, BLOCK), healthy_data)
                    os.write(release_w, b'R')
                    wait_read(notify_r, 0)  # a fresh read, not the old session's completion
                    os.write(release_w, b'R')
                    busy.receive((0, BLOCK), bad_data[0])
                    print('session: late completion rejected after same-client/same-block slot reuse')
                    return
                if stage == 'shutdown':
                    proc.terminate()
                    deadline = time.monotonic() + 10
                    while True:
                        text = (root / 'console.log').read_text()
                        logfile = root / 'logfile'
                        if logfile.exists():
                            text += logfile.read_text(errors='replace')
                        if 'Terminating core.' in text:
                            break
                        assert proc.poll() is None and time.monotonic() < deadline, 'shutdown did not reach core teardown'
                        time.sleep(0.01)
                    assert proc.poll() is None, 'daemon exited with an owned read still blocked'
                    os.close(release_w)
                    release_w = None
                    assert proc.wait(timeout=10) == 0, 'shutdown failed after releasing the read'
                    print('shutdown: blocked read drained and workers joined before core teardown')
                    return
                for peer in peers[1:4]:
                    peer.request([(0, BLOCK)])
                    peer.barrier()
                healthy.request([(0, BLOCK)])
                healthy.receive((0, BLOCK), healthy_data)
                # There must be no second read/open on the blocked drive, including
                # the alias directory, even though all its clients have requested data.
                assert not select.select([notify_r], [], [], 0)[0], 'same storage occupied multiple workers'
                peers[0].close()  # completion must discard a disconnected client's read
                healthy.request([(2 * BLOCK, 3 * BLOCK)])
                healthy.receive((2 * BLOCK, 3 * BLOCK), healthy_data)
                # Recover the drive. Each remaining request must complete exactly once.
                for index in range(4):
                    os.write(release_w, b'R')
                    if index < 3:
                        wait_read(notify_r, expected)
                for index in range(1, 4):
                    peers[index].receive((0, BLOCK), bad_data[index])
                assert proc.poll() is None, 'daemon exited after the delayed client disconnected'
                print(f'{stage}: healthy uploads continued, one permit across storage aliases, disconnect/recovery verified')
            except BaseException:
                print((root / 'console.log').read_text()[-5000:])
                raise
            finally:
                for fd in (release_w, release_r, notify_w):
                    if fd is not None:
                        os.close(fd)
                for peer in peers:
                    peer.close()
                if ec:
                    ec.sock.close()
                if proc:
                    stop_daemon(proc)
                os.close(notify_r)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary')
    parser.add_argument('fixture')
    parser.add_argument('--stage', choices=('read', 'open', 'session', 'packet-session', 'heap-buffer', 'mapped-copy', 'mapped-fault', 'shutdown', 'fairness', 'discovery', 'open-failure'))
    args = parser.parse_args()
    for stage in ([args.stage] if args.stage else ('read', 'open', 'session', 'packet-session', 'heap-buffer', 'mapped-copy', 'mapped-fault', 'shutdown', 'fairness', 'discovery', 'open-failure')):
        retry_port_collisions(run, str(Path(args.binary).resolve()), str(Path(args.fixture).resolve()), stage)
