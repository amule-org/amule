#!/usr/bin/env python3
# Copyright (c) 2026 aMule Team
# SPDX-License-Identifier: GPL-2.0-or-later
"""A saturated download limit must not delay the hello of a client we do not download from."""
import hashlib
from pathlib import Path
import socket
import sys
import tempfile
import time

from AllSearchIntegrationTest import C, free_ports, retry_port_collisions, start_daemon, stop_daemon
from DaemonConfig import write_amule_conf
from DisplayRatesIntegrationTest import DisplayRateMirror
from DownloadDisplayRatesIntegrationTest import Sources, hello
from ED2KLinkIntegrationTest import add_link
from UploadRefillIntegrationTest import packet, send

LIMIT = 20  # KiB/s
PROBES = 10
# On Linux a hello read through the saturated budget took about 0.3 s; an unthrottled one a few ms.
MAX_ANSWER = 0.1


def answer_time(address, peer_port, index):
    with socket.socket() as sock:
        sock.bind((address, 0))
        sock.connect((address, peer_port))
        sock.settimeout(30)
        start = time.monotonic()
        send(sock, 0x01, hello(f'hello-probe-{index}'.encode(), address, sock.getsockname()[1]))
        while packet(sock)[1] != 0x4c:  # OP_HELLOANSWER
            pass
        return time.monotonic() - start


def run(binary):
    with tempfile.TemporaryDirectory(prefix='amule-download-limit-') as directory:
        root = Path(directory)
        for name in ('Incoming', 'Temp'):
            (root / name).mkdir()
        ec_port, peer_port = free_ports(2, peer_index=1)
        sources = Sources(1, hashlib.md5(b'download limit fixture').digest(), b'limit.bin')
        # Send far above the limit, so the download budget stays exhausted.
        sources.rate = 8 * LIMIT * 1024
        write_amule_conf(root, ec_port, Address=sources.address, Port=peer_port, UDPEnable=0, FilterLanIPs=0,
                         MaxDownload=LIMIT, MaxUpload=0)

        ec = proc = None
        try:
            proc, ec = start_daemon(binary, root, (ec_port, peer_port))
            assert add_link(ec, sources.link(8388608)) == C['EC_OP_NOOP']
            sources.start(peer_port)
            sources.wait_ready(lambda: (root / 'logfile').read_text()[-4000:])
            time.sleep(2)
            _, peers, _, downloading = DisplayRateMirror(ec).poll_downloads()
            # Otherwise the budget is not exhausted and the probes prove nothing.
            assert downloading == 1 and 0.5 < peers / (LIMIT * 1024) < 1.5, ('download not at the limit', peers)

            times = [answer_time(sources.address, peer_port, index) for index in range(PROBES)]
            assert max(times) < MAX_ANSWER, ('hello answered late', [round(t, 3) for t in times])
            assert not sources.errors, sources.errors
            print(f'PASS: {PROBES} hellos answered in at most {max(times) * 1000:.0f} ms '
                  f'while downloading at the {LIMIT} KiB/s limit')
        finally:
            sources.close()
            if ec:
                ec.sock.close()
            if proc:
                stop_daemon(proc, check=sys.exc_info()[0] is None)


if __name__ == '__main__':
    retry_port_collisions(lambda: run(str(Path(sys.argv[1]).resolve())))
