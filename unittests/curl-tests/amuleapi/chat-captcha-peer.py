#!/usr/bin/env python3
"""Offline CAPTCHA integration test. Runs isolated amuled/amuleapi instances.

Usage: python3 unittests/curl-tests/amuleapi/chat-captcha-peer.py --build-dir build
No installed services, real peers, or user configuration are used.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request


# Reuse the suite's EC codec, without its daemon-start or English-log helpers.
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests"))
from AllSearchIntegrationTest import EC, C, integer


def unused_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait_for(fn, description):
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        try:
            value = fn()
            if value:
                return value
        except (OSError, urllib.error.URLError):
            pass
        time.sleep(0.1)
    raise AssertionError("Timed out: " + description)


def bitmap():
    data = bytearray(62 + 12 * 36)
    struct.pack_into("<2sIHHI", data, 0, b"BM", len(data), 0, 0, 62)
    struct.pack_into("<IiiHHIIiiII", data, 14, 40, 92, 36, 1, 1, 0, 432, 0, 0, 0, 0)
    data[58:62] = bytes([255, 255, 255, 0])
    return bytes(data)


def read_exact(sock, size):
    result = b""
    while len(result) < size:
        chunk = sock.recv(size - len(result))
        if not chunk:
            raise AssertionError("Peer connection closed")
        result += chunk
    return result


def packet(sock):
    protocol, size = struct.unpack("<BI", read_exact(sock, 5))
    payload = read_exact(sock, size)
    return protocol, payload[0], payload[1:]


def send_packet(sock, protocol, opcode, payload):
    sock.sendall(struct.pack("<BIB", protocol, len(payload) + 1, opcode) + payload)


def run(build, work, daemon_override=None, api_override=None):
    ec_port, http_port, tcp_port = (unused_port() for _ in range(3))
    daemon_dir, api_dir = work / "daemon", work / "api"
    daemon_dir.mkdir()
    api_dir.mkdir()
    # Configs contain credentials and amuleapi enforces owner-only permissions.
    # Set the mode at creation so permissive CI umasks cannot expose them.
    (daemon_dir / "amule.conf").touch(mode=0o600, exist_ok=False)
    (api_dir / "amuleapi.conf").touch(mode=0o600, exist_ok=False)
    (daemon_dir / "amule.conf").write_text(f"""[eMule]
Nick=CAPTCHA test
Port={tcp_port}
UDPDisable=1
ConnectToKad=0
ConnectToED2K=0
FilterLanIPs=0
IPFilterAutoLoad=0
IncomingDir={daemon_dir / 'incoming'}
TempDir={daemon_dir / 'temp'}
[ExternalConnect]
AcceptExternalConnections=1
ECAddress=127.0.0.1
ECPort={ec_port}
ECPassword={hashlib.md5(b'captcha-test').hexdigest()}
""")
    (api_dir / "amuleapi.conf").write_text(f"""[EC]
Host=127.0.0.1
Port={ec_port}
Password=captcha-test
[Server]
BindAddress=127.0.0.1
Port={http_port}
StaticRoot={Path(__file__).resolve().parents[3] / 'src/webapi/static'}
""")
    daemon = daemon_override or build / "src/amuled"
    api = api_override or build / "src/webapi/amuleapi"
    processes = []
    logs = []
    stream = None
    ec = None
    try:
        for executable, directory, args in [(daemon, daemon_dir, ["-o"]),
                                             (api, api_dir, ["--foreground"])]:
            log = (work / (executable.name + ".log")).open("w")
            logs.append(log)
            if executable == api:
                subprocess.run([str(api), "--config-dir", str(api_dir),
                                "--set-admin-pass", "captcha-admin"],
                               stdout=log, stderr=log, check=True, timeout=20)
            processes.append(subprocess.Popen([str(executable), "--config-dir", str(directory), *args],
                                               stdout=log, stderr=log))
            if executable == daemon:
                def ec_ready():
                    with socket.create_connection(("127.0.0.1", ec_port), timeout=0.2):
                        return True
                wait_for(ec_ready, "daemon EC listener")
        root = f"http://127.0.0.1:{http_port}/api/v1"
        token = ""

        def request(path, body=None, method=None):
            headers = {"Content-Type": "application/json"}
            if token:
                headers["Authorization"] = "Bearer " + token
            req = urllib.request.Request(root + path, headers=headers, method=method,
                                         data=None if body is None else json.dumps(body).encode())
            with urllib.request.urlopen(req, timeout=3) as resp:
                data = resp.read()
                return json.loads(data) if data else None

        wait_for(lambda: request("/health"), "API startup")
        token = request("/auth/login?include_token=true", {"password": "captcha-admin"})["token"]
        stream = urllib.request.urlopen(urllib.request.Request(root + "/events",
            headers={"Authorization": "Bearer " + token}), timeout=15)
        ec = EC(ec_port, password="captcha-test")
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen()
            listener.settimeout(10)
            peer_port = listener.getsockname()[1]
            address = f"127.0.0.1:{peer_port}"
            request(f"/chats/{address}/messages", {"text": "hello"})
            sock, _ = listener.accept()
            with sock:
                sock.settimeout(10)
                protocol, opcode, hello = packet(sock)
                assert (protocol, opcode) == (0xE3, 0x01)
                # Reply with a stable hash, routable high ID, name and CAPTCHA capability.
                name = b"CAPTCHA peer"
                tags = (b"\x02\x01\x00\x01" + struct.pack("<H", len(name)) + name +
                        b"\x03\x01\x00\xfe" + struct.pack("<I", 1 << 11))
                reply = (bytes.fromhex("0123456789abcdef0123456789abcdef") +
                         struct.pack("<IHI", 0x0100007F, peer_port, 2) + tags + bytes(6))
                send_packet(sock, 0xE3, 0x4C, reply)

                def next_message(expected):
                    while True:
                        proto, op, payload = packet(sock)
                        if (proto, op) == (0xE3, 0x4E):
                            assert payload[2:].decode() == expected
                            return
                next_message("hello")
                def rejected_answer(challenge_id, status=409, target=None):
                    try:
                        request(f"/chats/{target or address}/messages", {"text": "stale answer", "captcha_id": challenge_id})
                    except urllib.error.HTTPError as error:
                        assert error.code == status
                    else:
                        raise AssertionError("Stale CAPTCHA answer was sent as a chat message")

                history = lambda: request(f"/chats/{address}/messages")["messages"]
                wait_for(lambda: len(history()) == 1, "initial transcript")
                bmp = bitmap()
                # Result before any solution and malformed images must not reach the transcript.
                send_packet(sock, 0xC5, 0xA6, b"\x00")
                bad = bytearray(bmp)
                bad[18:22] = struct.pack("<I", 1000000)
                send_packet(sock, 0xC5, 0xA5, b"\x00" + bad)
                time.sleep(1.5)
                assert len(history()) == 1

                for status, answer in [(1, "wrong"), (0, "ABCD")]:
                    before = len(history())
                    send_packet(sock, 0xC5, 0xA5, b"\x00" + bmp)
                    messages = wait_for(lambda: (m if len(m := history()) == before + 1 else None),
                                        "CAPTCHA challenge")
                    challenge = messages[-1]
                    assert challenge["type"] == "captcha_challenge"
                    assert base64.b64decode(challenge["captcha_image"], validate=True) == bmp
                    # A reconnecting GUI has already consumed this transcript id.
                    # Pending state must still carry the image with an empty delta.
                    op, tags = ec.call(C["EC_OP_GET_CHAT_SESSIONS"],
                        [integer(C["EC_TAG_CHAT_MSG_ID"], challenge["id"])])
                    assert op == C["EC_OP_CHAT_SESSIONS"]
                    fields = tags[C["EC_TAG_CHAT_SESSION"]][1]
                    assert C["EC_TAG_CHAT_MESSAGE"] not in fields
                    assert fields[C["EC_TAG_CHAT_CAPTCHA_ID"]][0] == challenge["id"]
                    encoded = fields[C["EC_TAG_CHAT_CAPTCHA_IMAGE"]][0].rstrip(b"\0")
                    assert base64.b64decode(encoded, validate=True) == bmp
                    session_row = request("/chats")["chats"][0]
                    assert session_row["pending_captcha_id"] == challenge["id"]
                    assert base64.b64decode(session_row["pending_captcha_image"], validate=True) == bmp
                    # Acknowledging the image suppresses it, independently of the cursor.
                    _, acknowledged = ec.call(C["EC_OP_GET_CHAT_SESSIONS"],
                        [integer(C["EC_TAG_CHAT_MSG_ID"], challenge["id"]),
                         integer(C["EC_TAG_CHAT_CAPTCHA_ID"], challenge["id"])])
                    assert C["EC_TAG_CHAT_CAPTCHA_IMAGE"] not in acknowledged[C["EC_TAG_CHAT_SESSION"]][1]
                    # Duplicate challenges are rejected until this one is answered.
                    send_packet(sock, 0xC5, 0xA5, b"\x00" + bmp)
                    time.sleep(1.2)
                    assert len(history()) == before + 1
                    rejected_answer(challenge["id"] + 1)
                    for bad_id in [0, -1, 1.5, 4294967296, "1"]:
                        rejected_answer(bad_id, 400)
                    request(f"/chats/{address}/messages", {"text": answer, "captcha_id": challenge["id"]})
                    next_message(answer)
                    rejected_answer(challenge["id"])
                    _, tags = ec.call(C["EC_OP_GET_CHAT_SESSIONS"],
                        [integer(C["EC_TAG_CHAT_MSG_ID"], challenge["id"])])
                    fields = tags[C["EC_TAG_CHAT_SESSION"]][1]
                    assert fields[C["EC_TAG_CHAT_CAPTCHA_ID"]][0] == 0
                    assert C["EC_TAG_CHAT_CAPTCHA_IMAGE"] not in fields
                    # A second ordinary message while the result is delayed must neither
                    # assert nor discard the pending result.
                    extra = 0
                    if status == 0:
                        request(f"/chats/{address}/messages", {"text": "still waiting"})
                        next_message("still waiting")
                        extra = 1
                    # Invalid result values must not reset the pending solution state.
                    send_packet(sock, 0xC5, 0xA6, b"\xff")
                    send_packet(sock, 0xC5, 0xA6, bytes([status]))
                    messages = wait_for(lambda: (m if len(m := history()) == before + 3 + extra else None),
                                        "CAPTCHA result")
                    assert messages[-1]["type"] == "captcha_result"
                    assert messages[-1]["captcha_status"] == status
                    if status == 1:
                        request(f"/chats/{address}/messages", {"text": "retry"})
                        next_message("retry")
                        wait_for(lambda: len(history()) == before + 4, "retry message")
                # A challenge after success, without a new outgoing message, is unsolicited.
                count = len(history())
                send_packet(sock, 0xC5, 0xA5, b"\x00" + bmp)
                time.sleep(1.5)
                assert len(history()) == count
                rejected_answer(challenge["id"])
                # A new message starts a new exchange, including the exhausted status.
                request(f"/chats/{address}/messages", {"text": "one more"})
                next_message("one more")
                wait_for(lambda: len(history()) == count + 1, "new message")
                send_packet(sock, 0xC5, 0xA5, b"\x00" + bmp)
                wait_for(lambda: len(history()) == count + 2, "last challenge")
                request(f"/chats/{address}/messages", {"text": "wrong again"})
                next_message("wrong again")
                send_packet(sock, 0xC5, 0xA6, b"\x02")
                wait_for(lambda: len(history()) == count + 4, "exhausted result")
                transcript = history()
                assert transcript[-1]["captcha_status"] == 2
                # The live event and REST writers must agree for every CAPTCHA entry.
                expected = {m["id"]: m for m in transcript if m.get("type", "").startswith("captcha_")}
                event = ""
                while expected:
                    raw = stream.readline()
                    assert raw, "SSE stream ended before CAPTCHA events"
                    line = raw.decode().strip()
                    if line.startswith("event:"):
                        event = line[6:].strip()
                    elif line.startswith("data:") and event == "chat_message":
                        message = json.loads(line[5:])["message"]
                        if message["id"] in expected:
                            assert message == expected.pop(message["id"])
                # A pending image must not turn the next connection's first message
                # into a solution to the previous connection's challenge.
                count = len(history())
                request(f"/chats/{address}/messages", {"text": "before disconnect"})
                next_message("before disconnect")
                wait_for(lambda: len(history()) == count + 1, "message before disconnect")
                send_packet(sock, 0xC5, 0xA5, b"\x00" + bmp)
                wait_for(lambda: len(history()) == count + 2, "challenge before disconnect")
                expired_id = history()[-1]["id"]
                sock.shutdown(socket.SHUT_RDWR)
                sock.close()
                wait_for(lambda: any(c["connected"] is False for c in request("/chats")["chats"]),
                         "disconnected peer")
                wait_for(lambda: request("/chats")["chats"][0]["pending_captcha_id"] is None,
                         "expired REST pending challenge")
                assert request("/chats")["chats"][0]["pending_captcha_image"] is None
                rejected_answer(expired_id)
                # Expiry has no transcript message but must reach live viewers.
                saw_pending = False
                while True:
                    raw = stream.readline()
                    assert raw, "SSE stream ended before CAPTCHA expiry"
                    line = raw.decode().strip()
                    if line.startswith("event:"):
                        event = line[6:].strip()
                    elif line.startswith("data:") and event == "chat_session_updated":
                        pending = json.loads(line[5:])["pending_captcha_id"]
                        if pending == expired_id:
                            saw_pending = True
                        elif pending is None and saw_pending:
                            break
                request(f"/chats/{address}/messages", {"text": "after reconnect"})
                sock, _ = listener.accept()
                with sock:
                    sock.settimeout(10)
                    assert packet(sock)[:2] == (0xE3, 0x01)
                    send_packet(sock, 0xE3, 0x4C, reply)
                    next_message("after reconnect")
                    wait_for(lambda: len(history()) == count + 3, "message after reconnect")
                    send_packet(sock, 0xC5, 0xA5, b"\x00" + bmp)
                    messages = wait_for(lambda: (m if len(m := history()) == count + 4 else None),
                                        "fresh challenge after reconnect")
                    assert messages[-1]["type"] == "captcha_challenge"
                    last_challenge = messages[-1]["id"]
                    request(f"/chats/{address}", method="DELETE")
                    rejected_answer(last_challenge)
                    rejected_answer(last_challenge, target=f"127.0.0.1:{unused_port()}")
                    # Session closure must not allow the peer to recreate a transcript.
                    send_packet(sock, 0xC5, 0xA5, b"\x00" + bmp)
                    time.sleep(1.2)
                    assert not request("/chats")["chats"]
                    # Reopening the same live peer must start fresh, not submit an
                    # answer to the challenge from the closed session.
                    request(f"/chats/{address}/messages", {"text": "reopened"})
                    next_message("reopened")
                    wait_for(lambda: len(history()) == 1, "reopened transcript")
                    send_packet(sock, 0xC5, 0xA5, b"\x00" + bmp)
                    messages = wait_for(lambda: (m if len(m := history()) == 2 else None),
                                        "new challenge after closing and reopening")
                    rejected_answer(last_challenge)
                    assert messages[-1]["id"] != last_challenge
                    pending_id = messages[-1]["id"]
                    for index in range(205):
                        text = f"incoming {index}".encode()
                        send_packet(sock, 0xE3, 0x4E, struct.pack("<H", len(text)) + text)
                    wait_for(lambda: len(history()) == 200 and history()[-1]["text"] == "incoming 204",
                             "pending challenge evicted from history")
                    assert all(m["id"] != pending_id for m in history())
                    row = request("/chats")["chats"][0]
                    assert row["pending_captcha_id"] == pending_id
                    assert base64.b64decode(row["pending_captcha_image"], validate=True) == bmp
                    _, tags = ec.call(C["EC_OP_GET_CHAT_SESSIONS"],
                        [integer(C["EC_TAG_CHAT_MSG_ID"], row["last_message_id"])])
                    fields = tags[C["EC_TAG_CHAT_SESSION"]][1]
                    assert C["EC_TAG_CHAT_MESSAGE"] not in fields
                    assert base64.b64decode(fields[C["EC_TAG_CHAT_CAPTCHA_IMAGE"]][0].rstrip(b"\0")) == bmp
                    request(f"/chats/{address}", method="DELETE")
        print("PASS: local peer challenge, image validation, retry, success, exhausted attempts, duplicate/unsolicited packets, REST, SSE, reconnect, close/reopen, delayed results and stale answers")
    except Exception:
        for log in logs:
            log.flush()
        for file in work.glob("*.log"):
            print(file.name + ":\n" + file.read_text()[-6000:])
        raise
    finally:
        if ec is not None:
            ec.sock.close()
        if stream is not None:
            stream.close()
        for process in reversed(processes):
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        for log in logs:
            log.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--amuled", type=Path, help="Explicit daemon executable (CTest)")
    parser.add_argument("--amuleapi", type=Path, help="Explicit API executable (CTest)")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="amule-captcha-test-") as directory:
        run(args.build_dir.resolve(), Path(directory),
            args.amuled.resolve() if args.amuled else None,
            args.amuleapi.resolve() if args.amuleapi else None)
