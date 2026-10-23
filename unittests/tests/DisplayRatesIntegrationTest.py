#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Read live total, peer and shared-file speeds from an isolated loopback upload.

The observer keeps EC delta state exactly as a remote GUI does. Polling continues after
requests drain, with the peer still connected, to catch missing idle-rate invalidation.
"""
import json
from pathlib import Path
import struct
import sys
import time

from AllSearchIntegrationTest import C, EC, exact, integer, retry_port_collisions, tag
from ShareWatcherMoveIntegrationTest import tag_list
from UploadRefillIntegrationTest import run


class PartialUpdateEC(EC):
    def __init__(self, port):
        super().__init__(port, capabilities=[tag(C['EC_TAG_CAN_PARTIAL_UPDATE'])])
        assert C['EC_TAG_CAN_PARTIAL_UPDATE'] in self.auth_tags, self.auth_tags


class DisplayRateMirror:
    def __init__(self, ec):
        self.ec = ec
        self.files = {}
        self.peers = {}

    def poll(self):
        payload = (bytes([C['EC_OP_GET_UPDATE']]) + struct.pack('!H', 1)
                   + integer(C['EC_TAG_DETAIL_LEVEL'], C['EC_DETAIL_INC_UPDATE']))
        self.ec.sock.sendall(struct.pack('!II', 0x20, len(payload)) + payload)
        flags, length = struct.unpack('!II', exact(self.ec.sock, 8))
        assert flags == 0x20, flags
        reply = exact(self.ec.sock, length)
        assert reply[0] == C['EC_OP_SHARED_FILES'], reply[0]
        tags, _ = tag_list(reply, 3, struct.unpack_from('!H', reply, 1)[0])
        for name, value, children in tags:
            if name in (C['EC_TAG_KNOWNFILE'], C['EC_TAG_PARTFILE']):
                fields = self.files.setdefault(int.from_bytes(value, 'big'), {})
                fields.update({key: int.from_bytes(data, 'big') for key, data, _ in children
                               if key in (C['EC_TAG_KNOWNFILE_UPLOAD_SPEED'],
                                          C['EC_TAG_KNOWNFILE_UPLOADING_COUNT'], C['EC_TAG_PARTFILE_SPEED'],
                                          C['EC_TAG_PARTFILE_SOURCE_COUNT_XFER'])})
                fields.update({key: float(data.rstrip(b'\0')) for key, data, _ in children
                               if key == C['EC_TAG_PARTFILE_SPEED_SMOOTHED']})
            elif name == C['EC_TAG_CLIENT']:
                present = set()
                for _, peer_id, fields in children:
                    peer_id = int.from_bytes(peer_id, 'big')
                    present.add(peer_id)
                    peer = self.peers.setdefault(peer_id, {})
                    peer.update({key: int.from_bytes(data, 'big') for key, data, _ in fields
                                 if key in (C['EC_TAG_CLIENT_UP_SPEED'], C['EC_TAG_CLIENT_DOWNLOAD_STATE'],
                                            C['EC_TAG_CLIENT_UPLOAD_SESSION'])})
                    peer.update({key: float(data.rstrip(b'\0')) for key, data, _ in fields
                                 if key == C['EC_TAG_CLIENT_DOWN_SPEED']})
                self.peers = {key: value for key, value in self.peers.items() if key in present}
        op, stats = self.ec.call(C['EC_OP_STAT_REQ'])
        assert op == C['EC_OP_STATS'], op
        total = stats[C['EC_TAG_STATS_UL_SPEED']][0]
        peers = sum(fields.get(C['EC_TAG_CLIENT_UP_SPEED'], 0) for fields in self.peers.values())
        files = sum(fields.get(C['EC_TAG_KNOWNFILE_UPLOAD_SPEED'], 0) for fields in self.files.values())
        uploading = sum(fields.get(C['EC_TAG_KNOWNFILE_UPLOADING_COUNT'], 0)
                        for fields in self.files.values())
        return total, peers, files, uploading

    def poll_downloads(self):
        self.poll()
        op, stats = self.ec.call(C['EC_OP_STAT_REQ'])
        assert op == C['EC_OP_STATS'], op
        peers = sum(fields.get(C['EC_TAG_CLIENT_DOWN_SPEED'], 0) * 1024 for fields in self.peers.values())
        files = sum(fields.get(C['EC_TAG_PARTFILE_SPEED'], 0) for fields in self.files.values())
        downloading = sum(fields.get(C['EC_TAG_PARTFILE_SOURCE_COUNT_XFER'], 0) for fields in self.files.values())
        return stats[C['EC_TAG_STATS_DL_SPEED']][0], peers, files, downloading

    def check_active(self, expected_rate, slots):
        # A 30-second total average is still far below the cap after two seconds.
        # Allow socket batching and the up-to-one-core-tick gap between requests.
        total, peers, files, uploading = self.poll()
        assert total > expected_rate * 0.5, ('total speed still uses a long average', total, expected_rate)
        assert peers > 0 and files > 0 and uploading == slots, (total, peers, files, uploading)
        assert abs(total - peers) <= max(65536, total * 0.35), (total, peers)
        assert peers == files, ('peer and file snapshots disagree', peers, files)


class DisplayRatesObserver:
    def __init__(self, ec):
        self.partial = PartialUpdateEC(ec.sock.getpeername()[1])
        self.mirrors = [DisplayRateMirror(ec), DisplayRateMirror(self.partial)]

    def check_active(self, expected_rate, slots):
        for mirror in self.mirrors:
            mirror.check_active(expected_rate, slots)

    def check_accuracy(self):
        # Compare displayed bytes/s against session byte growth over many daemon
        # ticks. Comparing peer/file totals alone misses a shared 300 ms bias.
        previous = [sum(peer.get(C['EC_TAG_CLIENT_UPLOAD_SESSION'], 0)
                        for peer in mirror.peers.values()) for mirror in self.mirrors]
        starts = [time.monotonic() for _ in self.mirrors]
        integrated = [[0.0, 0.0] for _ in self.mirrors]
        for _ in range(20):
            time.sleep(0.3)
            for index, mirror in enumerate(self.mirrors):
                total, peers, _, _ = mirror.poll()
                now = time.monotonic()
                integrated[index][0] += total * (now - starts[index])
                integrated[index][1] += peers * (now - starts[index])
                starts[index] = now
        for index, mirror in enumerate(self.mirrors):
            transferred = sum(peer.get(C['EC_TAG_CLIENT_UPLOAD_SESSION'], 0)
                              for peer in mirror.peers.values()) - previous[index]
            assert transferred > 0, 'accuracy check missed active uploads'
            for name, displayed in zip(('total', 'peers'), integrated[index]):
                ratio = displayed / transferred
                assert 0.9 <= ratio <= 1.1, ('displayed upload rate disagrees with session growth', name, ratio)

    def close(self):
        self.partial.sock.close()

    def check_file_switch(self, sockets, file_hash, switched, received):
        from UploadRefillIntegrationTest import send
        for mirror in self.mirrors:
            assert mirror.poll()[1] > 0, 'switch test missed the recent upload window'
        for sock in sockets:
            send(sock, 0x4f, file_hash)  # OP_SETREQFILEID, retaining the upload slot
        deadline = time.monotonic() + 5
        while not all(event.is_set() for event in switched):
            assert time.monotonic() < deadline, 'peers did not receive the new file status'
            time.sleep(0.01)
        # A later accounting tick must not repopulate the new file's empty window
        # with bytes that were sent before the switch (daemon ticks are 300 ms).
        time.sleep(0.35)
        for mirror in self.mirrors:
            deadline = time.monotonic() + 5
            while True:
                total, peers, files, uploading = mirror.poll()
                accounted = sum(peer.get(C['EC_TAG_CLIENT_UPLOAD_SESSION'], 0)
                                for peer in mirror.peers.values())
                if accounted >= sum(received):
                    break
                assert time.monotonic() < deadline, ('switch discarded transfer accounting', accounted, received)
                time.sleep(0.01)
            assert uploading == len(sockets), 'file switch lost the upload slots'
            assert peers == files == 0, ('new share inherited old upload bytes', total, peers, files)

    def check_idle(self, received):
        deadline = time.monotonic() + 20
        previous = received.copy()
        idle_since = time.monotonic()
        while time.monotonic() < deadline:
            samples = [mirror.poll() for mirror in self.mirrors]
            if received != previous:
                previous = received.copy()
                idle_since = time.monotonic()
            elif time.monotonic() - idle_since >= 2:
                for sample in samples:
                    total, peers, files, uploading = sample
                    assert uploading > 0, 'idle test lost its upload slots'
                    assert total < 4096 and peers == files == 0, ('stale idle rates', sample)
                return
            time.sleep(0.1)
        raise AssertionError('outstanding loopback requests did not drain')


if __name__ == '__main__':
    print(json.dumps(retry_port_collisions(lambda: run(
        str(Path(sys.argv[1]).resolve()), 1, 2, 2560, check_display_rates=True))))
