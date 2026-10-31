# NAT-T UDP EAQC CAPS/CAPS_ACK

This document specifies aMule's experimental capability exchange for the eMuleAI
NAT-T UDP path. With `QuicNatT` on, aMule answers a valid CAPS with a CAPS_ACK,
subject to the rate limit below. It sends CAPS itself, and dials QUIC, only on a
server-vouched LowID rendezvous (an ed2k-server hole punch or a Lugdunum NAT
callback; see the sections below), where uTP is the fallback. On eMuleAI's own
direct NAT-T path aMule only responds: eMuleAI dials aMule's inbound QUIC
endpoint when it is the downloader, but as the uploader it opens a passive
endpoint and waits for a dial that aMule does not make on that path. The
exchange does not change the normal eD2k handshake. The QUIC dial has been
tested live between two aMule nodes; it has not yet been live-tested against
eMuleAI 1.6.

## Availability and discovery

The exchange is compile-time gated by `ENABLE_QUIC`, which defines
`AMULE_QUIC_TRANSPORT` for the core targets, and runtime gated by the
`amule.conf` key `/eMule/QuicNatT` (default **off**, no widget, no EC tag). It
is experimental. A build without QUIC support drops CAPS without responding and
logs the throttled unsupported-frame event. A QUIC build with `QuicNatT` off
silently ignores CAPS, CAPS_ACK and QUIC frames, and uses key frames only for uTP
streams (see [Key frame](#key-frame-quic-and-utp-datagram-obfuscation)). The
outbound CAPS and QUIC dial also need `ENABLE_NATT_SERVER_COORDINATION` and
`ENABLE_UTP`, which provide the rendezvous they run on.

The feature does not advertise a QUIC capability in HELLO or
`CT_MOD_MISCOPTIONS`. eMuleAI learns through `CAPS_ACK` instead. This avoids
marking aMule as a mod when it does not provide the readable `ET_MOD_VERSION`
that such an advertisement would imply.

## Datagram envelope

A CAPS or CAPS_ACK datagram is:

```text
OP_UDPRESERVEDPROT2 (0xB2) | type | EAQC payload (54 bytes)
```

The type byte is `0x02` for CAPS and `0x03` for CAPS_ACK. The EAQC payload is
exactly 54 bytes. CAPS is obfuscated with the dialled peer's hash and CAPS_ACK
with the sender hash from CAPS, when the sender has a public IP and UDP
obfuscation is enabled. `0xB2` is a reserved UDP protocol byte,
not an eD2k opcode; the type byte selects the frame. The constants are defined
in [`src/include/protocol/Protocols.h`](../src/include/protocol/Protocols.h).

## EAQC payload layout

All offsets below are relative to the first byte after the type byte. Integers
are little-endian where an integer encoding is shown; hashes are copied as
16-byte values.

| Offset | Size | Field | Encoding/value |
| ---: | ---: | --- | --- |
| 0 | 4 | Magic | ASCII `EAQC`; little-endian `uint32` `0x43514145` (`45 41 51 43`) |
| 4 | 1 | Version | `1` |
| 5 | 1 | Options | Capability bits; see below |
| 6 | 16 | Sender hash | Sender user hash |
| 22 | 16 | Expected peer hash | Recipient hash, or all zeroes as a wildcard; otherwise it must match the recipient |
| 38 | 16 | File hash | File hash; aMule echoes the request value in its ACK |

The encoder and decoder for this layout are in
[`src/QuicNattProtocol.h`](../src/QuicNattProtocol.h).

### Options

The CAPS and CAPS_ACK frames aMule sends use these bits:

| Bit | Name | Meaning |
| ---: | --- | --- |
| `0x40` | QUIC | QUIC NAT-T is available in this build |
| `0x08` | DIRECT_UDP_CALLBACK | Set only when `Kademlia::CPrefs::GetMyConnectOptions(false, true)` permits it |
| `0x80` | uTP | Set only when the build includes uTP transport |

The ACK sender hash is aMule's hash, its expected peer hash is the request's
sender hash, and its file hash is the request's file hash. A CAPS sent by aMule
carries aMule's hash, the peer's hash and an all-zero file hash, since every
dial that sends CAPS is a temporary rendezvous client with no requested file.

## Receiver behavior

For CAPS, the receiver:

1. requires the exact 54-byte payload length;
2. validates the `EAQC` magic and version `1`;
3. accepts an all-zero expected peer hash or requires an exact recipient-hash match;
4. rate-limits an otherwise valid response by the observed source-address scope;
5. sends one 54-byte CAPS_ACK when admitted.

The response limiter uses a three-second window and tracks at most 256 scopes.
A no-QUIC build logs and drops CAPS without sending an unsupported response.

A CAPS_ACK is never answered. It is accepted only when it decodes as above,
names a nonzero sender hash and nonzero options, and matches an unexpired CAPS
that aMule sent to the same peer hash, IP and UDP port; any other CAPS_ACK is
dropped. An accepted ACK records the peer as QUIC-compatible when the `0x40`
bit is set, and as incompatible otherwise, for 29 minutes.

The receive and response path is in
[`src/ClientUDPSocket.cpp`](../src/ClientUDPSocket.cpp), with the limiter policy
in [`src/NatRendezvousPolicy.h`](../src/NatRendezvousPolicy.h).

## Sender behavior

When a server-vouched LowID rendezvous is about to dial a peer with a known
user hash and UDP endpoint, aMule looks the peer up in its capability cache:

1. **Compatible:** dial QUIC at once.
2. **Incompatible or cooling down:** dial uTP.
3. **Unknown:** send CAPS and hold the dial for the answer. CAPS is sent at
   most three times, 500 ms apart. A compatible answer dials QUIC; an
   incompatible answer, or no answer within 1.5 s, dials uTP.

If a QUIC dial cannot start, or ends before its stream opens, the peer gets a
30-second cooldown and the rendezvous falls back to uTP.

Only a dial with a known peer hash can use QUIC: the ed2k-server requester
that received a 29- or 30-byte `0x61`, and a Lugdunum forward that carries the
user hash. The target-side dial triggered by `OP_HOLEPUNCH`, and its dial-back,
have no peer hash and always use uTP.

The first CAPS to a peer endpoint is recorded for 30 seconds; the first
matching ACK consumes it. The request ledger and the capability cache each
hold at most 256 entries. The file hash is informational: it is echoed and
recorded, but it does not choose roles.

The dial path is `CUpDownClient::TryServerVouchedLowIdUtpDial()` in
[`src/BaseClient.cpp`](../src/BaseClient.cpp), with the cache and hold-off
policy in [`src/QuicCapabilityCache.h`](../src/QuicCapabilityCache.h) and the
ledger in [`src/QuicCapsRequestLedger.h`](../src/QuicCapsRequestLedger.h).

## Key frame (QUIC and uTP datagram obfuscation)

A key frame tells the receiver which user hash to obfuscate its datagrams to the sender with:

```text
OP_UDPRESERVEDPROT2 (0xB2) | 0xFF | user hash (16 bytes)
```

The wire format is eMuleAI's. A receiver accepts a payload of at least 16 bytes, ignores any surplus and
rejects an all-zero hash. The type is `OP_NATT_FRAME_KEY` in
[`src/include/protocol/Protocols.h`](../src/include/protocol/Protocols.h).

What aMule does with it, in QUIC builds and only while `QuicNatT=1`:

- **Sending.** Every QUIC datagram is obfuscated with the peer's hash when it is known: the identity aMule
  dialled, or the hash a key frame announced. Obfuscation requires `ClientCryptLayerSupported` and a
  nonzero sender public IP (`GetPublicIP() != 0`); where the peer sees a different source IP, its sender-IP
  check may reject the datagram. A QUIC long-header packet is preceded by aMule's own key frame, at most
  one per endpoint every 10 s. aMule sends its key frame only where it can itself be obfuscated; eMuleAI
  also sends it in the clear when it has no key, which exposes the sender's user hash to anyone on the
  path. With either prerequisite unavailable, datagrams go plain and no key frame is sent.
- **Receiving.** A key frame is honoured only from an endpoint a rendezvous announced (the ed2k-server
  `0x61` or Lugdunum's forwarded callback) in the last 30 s, and only when that rendezvous did not provide
  a peer hash. The server-vouched hash remains authoritative when present. An accepted key frame can
  replace an earlier announced hash but never the identity aMule dialled; nothing authenticates the frame.
  A learned hash (including a server-vouched hash) expires after 120 s without outbound use; active
  datagrams refresh it. The 30 s expected-endpoint window controls only admission of unauthenticated key
  frames, not the lifetime of a vouched key for an active exchange. Distinct live server-vouched hashes
  for one endpoint are ambiguous: QUIC datagrams and key frames to that endpoint are dropped until the
  conflicting 30 s expectation expires, never downgraded to plaintext. The table holds at most 256 endpoints.
  Nothing is persisted: the hash selects an obfuscation key for one live exchange and is not an identity, which the
  `OP_HELLO` still establishes.

The datagram's own TLS 1.3 protection does not depend on this layer. It only hides the Initial and its
ALPN from the path. eMuleAI does not require it: its QUIC receiver drops a frame only when no session
matches. CAPS and CAPS_ACK are obfuscated with the recipient hash when the public-IP and obfuscation
prerequisites are met.

In uTP builds, key frames shorten the plaintext start of a uTP stream. A stream used to send its SYN,
SYN-ACK, hello and early STATEs in the clear, because its crypt parameters were set only at the hello.
The same prerequisites apply, and aMule's own key frame never leaves in the clear:

- **Dialling.** A dial that is obfuscated (the peer's hash and crypt support are known) sends aMule's key
  frame, obfuscated with the peer's hash, immediately before the SYN, at most once per endpoint every
  10 s. A plain dial sends no key frame.
- **Accepting.** A key frame from any endpoint is held for 5 s (at most 256 endpoints) for the next SYN
  from that endpoint. A live server-vouched hash for the endpoint outranks it, and conflicting vouchers
  key nothing. With a key, the SYN-ACK (which libutp sends before the stream is accepted) and every frame
  until the hello are obfuscated with it. A key never makes the stream count as obfuscated for
  "require obfuscation": that still depends on how the peer's SYN arrived.
- **Answering.** A plain SYN from an endpoint a rendezvous announced, with a key, is preceded by aMule's
  key frame (once per endpoint every 10 s), so a dialler that did not know aMule's hash can switch.
- **Switching.** A key frame from the peer of a plain dial still in its handshake keys that dial's later
  frames, with the server-vouched hash when there is one, else with the announced hash. This covers the
  ed2k-server punch, where both dials are plain but each side has the other's `0x61` hash.

In every case the hello replaces the key-frame hash with the crypt parameters it establishes.

A key frame is queued only while it can be obfuscated, and it is dropped rather than sent plain if
obfuscation lapses before it leaves (the public IP lost in between).
Ordinary client UDP keeps its existing plain fallback.

The decisions are in [`src/NatKeyFrame.h`](../src/NatKeyFrame.h), driven from
[`src/ClientUDPSocket.cpp`](../src/ClientUDPSocket.cpp) and
[`src/UtpLibraryAdapter.cpp`](../src/UtpLibraryAdapter.cpp).

## Scope and interoperability status

This exchange only negotiates capability information. It does not by itself
establish a QUIC connection: aMule dials QUIC only on a server-vouched LowID
rendezvous, after a compatible answer.

A future direct interoperability run requires eMuleAI to know aMule's UDP
endpoint, aMule to have a LowID, and eMuleAI itself to be firewalled. eMuleAI
must also lack a usable Kad buddy for this peer so it selects direct NAT
traversal instead of rendezvous. No live eMuleAI 1.6 validation has been
performed for this task.

## Manual interoperability runbook

This is a manual runbook, not a fully scripted recipe. The lab setup for
resolving or seeding the peer and suppressing its usable Kad buddy remains
unresolved and must be confirmed on the actual eMuleAI installation.

1. **Build the aMule target.** Configure CMake with `-DENABLE_QUIC=ON` (add
   `-DENABLE_UTP=ON` for the uTP capability bit), confirm that the target
   under test uses the resulting QUIC-enabled core, and set `QuicNatT=1`
   under `[eMule]` in its `amule.conf`.
   That build only shows that the build path was exercised; it is not
   interoperation evidence by itself.
2. **Confirm the eMuleAI side.** Its documented defaults are
   `EnableNatTraversal=1` and `NatTraversalProtocolMode=0` (prefer QUIC).
   `LogNatTraversalEvents=1` enables NAT-T event logging when available. Do
   not treat changing those settings as an aMule-repository procedure. Record
   both peers' actual UDP endpoints, and ensure eMuleAI knows the aMule
   endpoint.
3. **Check the topology before starting.** Confirm all of the following:
   - aMule is LowID;
   - eMuleAI is locally firewalled;
   - the eMuleAI peer has no usable Kad buddy for this peer, so it selects
     direct traversal;
   - the peer/source identity and aMule UDP endpoint are known and reachable
     to the extent required by the test installation.

   There is no verified command or API here for manually seeding the peer or
   forcing the no-buddy condition. Do not substitute random port forwarding;
   resolve those lab-specific conditions on the real installation first.
4. **Capture the exchange and result.** On eMuleAI, look for the exact event
   sequence `[NAT-T][CAPS] Sent CAPS`, then `[NAT-T][CAPS] Direct caps confirmed quic=... utp=...`.
   There is no unprompted eMuleAI ACK. On aMule, capture evidence that it
   received CAPS and emitted a 54-byte `OP_NATT_FRAME_CAPS_ACK`, then accepted
   the QUIC frame and completed TLS/EAQN1. Record timestamps, endpoint
   addresses, packet lengths, and the connection/test result. No live run has
   yet produced this evidence.

### Blockers and pass criteria

A CAPS_ACK only negotiates capability; it does not create a NAT pinhole,
perform rendezvous or hole punching, or make a connection. On eMuleAI's
direct NAT-T path aMule has no outbound role. File-hash role gating is not
implemented: the file hash is echoed but not used to choose roles, so on that
path an eMuleAI uploader still gets QUIC advertised and waits for a dial that
never comes. Do not claim PASS unless a real packet trace and the
corresponding connection/test logs show the CAPS exchange followed by the
QUIC/TLS/EAQN1 result.

## ed2k-server LowID hole punch

ed2k-server coordinates a hole punch between two LowID clients with
`OP_LOWID_HOLEPUNCH_REQUEST` (`0x60`), `OP_LOWID_HOLEPUNCH_INFO` (`0x61`),
`OP_LOWID_HOLEPUNCH_FAIL` (`0x62`) and the UDP keepalive `OP_SERVER_NATT_KEEPALIVE`
(`0x9F`) (eMule wiki "NatT protocol"; ed2k-server README "Implementing a compatible
client", 0.9.84 `39b9d85`). Since 0.9.84 it announces support with `OP_IDCHANGE`
TCP flag `0x20000` (`SRV_TCPFLG_LOWID_HOLEPUNCH`), and always sets it.

aMule implements this only when both `ENABLE_NATT_SERVER_COORDINATION` and `ENABLE_UTP`
are built (**experimental**, off by default; default and `ENABLE_IPV6` builds are unaffected)
and the runtime setting below is also on. With NAT-T compiled but uTP absent, aMule sends
none of `0xF9`, `0x9F` or `0x60` and cannot act on `0x61`:

- runtime opt-in: `thePrefs::IsLowIdHolePunchEnabled()` (`amule.conf` key
  `/eMule/LowIdHolePunch`, default **off**, no widget, no EC tag). It gates the `0xF9` login
  tag, the `0x9F` keepalive, sending `0x60` and keeping LowID sources, independently of
  `LugdunumNatCallback` -- a server is either Lugdunum or ed2k-server, never both
  (`NatServerHolePunch::ShouldRequestLowIdHolePunch()`'s `serverIsLugdunumActive` gate);
- login opt-in: `CT_EMULE_UDPPORTS` (`0xF9`), value `(kadUDPPort << 16) | clientUDPPort`
  (`ServerConnect.cpp`). aMule shares one UDP socket for Kad and the client protocol, so both
  halves carry `thePrefs::GetEffectiveUDPPort()`. Sent even when we never send `0x60`
  ourselves: it is what lets another LowID client's `0x60` name us as its target
  (ed2k-server refuses a target with no known UDP port);
- capability flag: `CServer::SupportsLowIdHolePunch()` reads `OP_IDCHANGE`'s TCP flags
  (`NatServerHolePunch::ServerAcceptsHolePunchRequest()`), read directly off `cur_server` --
  unlike Lugdunum's UDP-only fields, the TCP flags are already live there, so no server-list
  copy is needed;
- keepalive `OP_SERVER_NATT_KEEPALIVE` (`0x9F`): sent once as soon as the connection reaches
  `CS_CONNECTED` (`CServerConnect::ConnectionEstablished()`), and then roughly every 60 s from
  `CamuleApp::OnCoreTimer`, no new thread. `CServerConnect::TrySendLowIdHolePunchKeepalive()`
  sends our 16-byte user hash from the client UDP socket (`theApp->clientudp`, not the server
  link) to the server's IP at TCP port + 4, plain, or obfuscated to its obfuscation UDP port on an
  extension server (see below), only while we are LowID and `thePrefs::GetEffectiveUDPPort()` is
  nonzero (`NatServerHolePunch::ShouldSendKeepalive()`)
  -- ed2k-server refuses a HighID target (`FAIL_TARGET_IS_HIGHID`), and a disabled UDP port has
  no socket to send from;
- sending `0x60`: where a LowID-to-LowID contact is declined in `TryToContact()`, next to
  `TrySendLugdunumNatCallbackRequest()`, `CUpDownClient::TrySendLowIdHolePunchRequest()` sends
  `<target_id 4><our_udp_port 2>` over the server TCP link
  (`NatServerHolePunch::ShouldRequestLowIdHolePunch()`), at most once per source, and only when
  `thePrefs::GetEffectiveUDPPort()` is nonzero. Up to 8 requests may be pending per server, one
  per target ID; each expires after 30 s or is released by a `0x62` for its target ID. A source
  whose request found no room is asked again from the download queue's `DS_LOWTOLOWIP` pass;
- keeping LowID sources: ed2k-server sends LowID sources to any LowID requester
  unconditionally (confirmed against its `get_sources.rs`: no opt-in flag gates it, unlike
  Lugdunum). `CPartFile::AddSources()` therefore keeps one from our current,
  hole-punch-announcing server with the setting on
  (`NatServerHolePunch::ShouldKeepLowIdServerSource()`), independently of the Lugdunum check
  on the same call;
- receiving `0x61` on the server TCP link (`ServerSocket.cpp`): after validating the advertised
  IP against LAN, IP-filter and ban policies, register its peer endpoint as expected for 30 s,
  then send an empty `OP_HOLEPUNCH` (`0xA1`, `OP_EMULEPROT`) from the client UDP socket to it.
  The trailing `peer_id` of a 29-byte `0x61` (ed2k-server 0.9.84 and later; read only when the
  payload is 29 bytes or more) is the server ID of the peer described. When it names a target
  of one of our pending `0x60` requests, this side is the **requester** and dials: QUIC or
  uTP, chosen as in [Sender behavior](#sender-behavior), which may hold the dial up to 1.5 s
  for a CAPS answer; uTP when `QuicNatT` is off.
  Any other `0x61`, including a 25-byte one, leaves this side the **target**: it punches and
  waits. A 30-byte `0x61` (ed2k-server 0.9.85, between two clients using the hole-punch
  extension) ends with `peer_crypt`, the peer's `(CT_SERVER_FLAGS >> 9) & 7`. Every dial a
  `0x61` leads to, the requester's at once and the target's when the punch arrives, carries its
  hash and, when present, these crypt options (`CUpDownClient::SetConnectOptions()`), so a peer
  that requires obfuscation accepts it and the uTP dial is obfuscated from its SYN.
  Never write the `0x61` hash or address into a download source. The peer's `OP_HELLO`
  (or, for a dial of ours, its `OP_HELLOANSWER`) establishes identity, and the existing
  client-list matching attaches the stream to the LowID source. Repeated `0x61` for the same
  hash are ignored for 10 s and server-directed actions are limited to 20 per minute;
- receiving `OP_HOLEPUNCH` from a recently expected IP: accept it only once. A peer we already
  dialled needs nothing more when the punch comes from the port we dialled, and a dial to its
  **source** UDP port when it comes from another, which is the NAT's real mapping. A peer we
  did not dial is dialled back after 3 s, only if that peer (its IP and TCP port) has not
  connected by then, which normally leaves a single connection. The ordinary uTP connection
  and `OP_HELLO` perform the rest; `ShouldKeepFoundUtp()` remains the collision backstop, and when it swaps
  the stream under a source that had already sent its file request, the request is sent again
  on the kept stream. No TCP fallback is available between two LowID peers;
- receiving `0x62`: log the failure and release the matching outstanding target ID.

### Hole-punch extension (ed2k-server 0.9.85)

The extension (andrey23127/ed2k-server#39) lets the server hand out the UDP port from a fresh
`0x9F` at punch time, not from the last standing keepalive. It applies only between two clients
that announced it to a server that sets `0x100000` (`SRV_TCPFLG_HOLEPUNCH_EXTENSION`) in its
`OP_IDCHANGE` TCP flags; every other pair gets the punch above unchanged.

- announcing: `CT_SERVER_FLAGS` carries `0x100000` (`SRVCAP_HOLEPUNCH_EXTENSION`), always with
  Lugdunum's `0x1000`, only when both `LowIdHolePunch` and `LugdunumNatCallback` are on and uTP
  is built (`NatServerHolePunch::ShouldAnnounceHolePunchExtension()`). The server wakes an
  extension target with Lugdunum's `0x37`, which only the Lugdunum path serves. The session
  uses the extension when the server also set `0x100000` and answered our `0x1000` opt-in
  (`CServerConnect::IsHolePunchExtensionActive()`);
- `0x9F`: on such a server every `0x9F`, the standing keepalive included, is obfuscated the
  server way once its UDP key is known, and sent to its obfuscation UDP port from the client UDP
  socket. The server ignores a session's plain `0x9F` after its first obfuscated one. Without the
  key it stays plain, and an older server always gets it plain
  (`NatServerHolePunch::ShouldObfuscateKeepalive()`). One `0x9F` goes with each `0x60`;
- woken by `0x37`: send `0x9F` at 0, 0.5 and 1 s and `OP_HOLEPUNCH` to the requester's
  endpoint, with no dial and no reverse callback. The `0x61` that follows (30 bytes, with
  `peer_crypt`) decides who dials, as above. A `0x37` can also come from a Lugdunum `0xA6`, which
  no `0x61` follows: if none names the requester within 5 s (the server waits 3 s, then resends at
  +1.2 s and +3 s), aMule dials the Lugdunum way, as it would on any Lugdunum server. Any `0x61`
  that names the requester answers the wake, even one the action limit or the duplicate window
  keeps us from acting on. At most 16 wakes wait at once (`NatServerHolePunch::PendingWakeSet`).
  A UDP `0xA7` from our own server answers a Lugdunum `0xA6` of ours and is dialled at once,
  except a cross-server wake (requester ID 0, below), which the server sends by UDP when we asked
  for a callback ourselves less than 20 s before.

### Cross-server NAT callback (ed2k-server 0.9.86)

Two LowIDs on different servers meet through the target's own server, without a registry
(andrey23127/ed2k-server#40). The source's server address and LowID come with the source, from
source exchange or a server's source answer. Everything below needs the hole-punch extension's
announce condition (`LowIdHolePunch` and `LugdunumNatCallback` on, uTP built); with it off, or
without `ENABLE_NATT_SERVER_COORDINATION`, nothing changes.

- which server: only one that is in our own server list, sets `0x100000`
  (`SRV_UDPFLG_FOREIGN_NAT_CALLBACK`) in the UDP flags of its status reply and gave us its UDP key
  for our address (`NatServerHolePunch::ServerTakesForeignCallback()`,
  `CServerConnect::ForeignCallbackServer()`). A server address learnt from a peer is never asked;
- keeping sources: while we are LowID, a LowID source of such a server survives
  `CPartFile::CanAddSource()`, from source exchange or from another server's answer
  (`NatServerHolePunch::ShouldKeepLowIdServerSource()`);
- requester: where a LowID-to-LowID contact is declined in `TryToContact()`, an odd LowID source
  on such a server gets `OP_NAT_CALLBACKREQUEST` (`0xA6`) `<target_id 4><0 4><crypt 1>`, server-key
  obfuscated, from the client UDP socket to that server's obfuscation UDP port. Requester ID 0
  marks it foreign; `crypt` is our `(CT_SERVER_FLAGS >> 9) & 7`. Per server we send a burst of 5,
  then one every 2 s, and at most 30 different targets in 10 minutes, below the server's own
  limits. A request waits 30 s for its answer; a target that never answers is not asked again for
  10 minutes (`NatServerHolePunch::ForeignCallbackRequests`);
- requester, answer: the server's `0xA7` is accepted only from that server's IP, decrypted with
  its key, exactly 27 bytes (`<target_ip 4><target_udp_port 2><target_id 4><crypt 1><hash 16>`),
  for a pending target, and with the hash source exchange gave us for the source when it did.
  Only the first copy is used; the server resends it at +1.2 s and +3 s. The requester punches
  and dials, as on the server punch: QUIC first, uTP as the fallback, keyed with the target's
  hash and crypt options;
- target: on a `0x100000` server, a `0x37` with requester ID 0, or a `0xA7` with requester ID 0
  when we sent an `0xA6` ourselves less than 20 s before, comes from a client logged in elsewhere.
  No `0x61` follows it: the target punches the requester's endpoint, sends its `0x9F` burst and
  waits to be dialled, with no fallback dial of its own. On a server without `0x100000`,
  requester ID 0 keeps its Lugdunum meaning, a reverse callback;
- not sent: when we or the source (by its crypt options from source exchange) require the crypt
  layer, since the server answers nothing then, and without the server's UDP key for our current
  IP (`CServer::GetServerKeyUDP()` is 0 once our public IP changed);
- retries: a request the rate cap or a cooldown held back is asked again from the download
  queue's `DS_LOWTOLOWIP` pass, at most once per 30 s per source. A target whose answer was taken
  also cools down for 10 minutes, so a dial that fails is not asked again at once. An answer the
  action limit refuses is not taken, so the server's resend can still be used.

The upload-side callback this path also allows (`OP_HELLO` carries the peer's server address and
ID) is not implemented.

The runtime setting remains off by default.

## Lugdunum NAT callback (eserver 17.15+)

Lugdunum servers relay a NAT callback between two LowID clients on the same
server (eMule wiki, "NatT protocol", section "eServer (Lugdunum)"). A client
opts in with `CT_SERVER_FLAGS` bit `0x1000` (`SRVCAP_NATTRAVERSAL`); the
server then assigns an odd LowID and appends a "your port" field to
`OP_IDCHANGE`. The server announces support with TCP flag `0x1000`
(`SRV_TCPFLG_NATTRAVERSAL`). That bit alone is not proof: another server uses
`0x1000`/`0x2000` unofficially in its flags, so the 24-byte `OP_IDCHANGE`
answering an opt-in is the stronger signal. The wiki also lists a UDP flag,
`0x00010000`, but public Lugdunum servers do not set it (they report `0x17FB`),
and eNode uses that bit for meta search; aMule does not define it.

| Opcode | Value | Direction | Payload |
| --- | --- | --- | --- |
| `OP_NAT_CALLBACKREQUEST` | `0xA6` | client UDP socket to server UDP | `<target ID 4><requester ID 4>` |
| `OP_NAT_CALLBACKREQUEST`, cross-server (ed2k-server 0.9.86) | `0xA6` | client UDP socket to the source's server UDP | `<target ID 4><0 4><crypt 1>` |
| `OP_NAT_CALLBACKREQUESTED_UDP` | `0xA7` | server UDP to client UDP socket | `<seen IP 4><seen UDP port 2><requester ID 4>[<obfuscation settings 1><user hash 16>]` |
| `OP_NAT_CALLBACKREQUESTED_UDP`, cross-server answer (ed2k-server 0.9.86) | `0xA7` | the source's server UDP to client UDP socket | `<target IP 4><target UDP port 2><target ID 4><crypt 1><user hash 16>`, exactly 27 bytes |
| `OP_NAT_CALLBACKREQUESTED` | `0x37` | server TCP to client | as `0xA7` |

`OP_IDCHANGE` with the extra field is
`<ID 4><flags 4><server TCP port 4><reported IP 4><obfuscation TCP port 4><your port 4>`.
For a LowID, "your port" is our TCP port as the server sees it, after any NAT.

The Lugdunum callback relay remains gated by `ENABLE_NATT_SERVER_COORDINATION` (**experimental**, off by
default; default and `ENABLE_IPV6` builds are unaffected) and the runtime setting below. Acting on a
forwarded callback by dialing additionally requires `ENABLE_UTP` (`AMULE_UTP_TRANSPORT`) in the
`amule` or `amuled` target:

- runtime opt-in: `thePrefs::IsLugdunumNatCallbackEnabled()` (`amule.conf` key
  `/eMule/LugdunumNatCallback`, default **off**, no widget, no EC tag). It gates the login
  opt-in, sending `OP_NAT_CALLBACKREQUEST` and keeping LowID sources, so turning on
  `ENABLE_NATT_SERVER_COORDINATION` alone (e.g. via `ENABLE_ALL_EXPERIMENTAL` in CI or tester
  builds) does not enrol a build on a live server. Receiving and logging `0xA7`/`0x37` needs no
  gate: those only arrive after opting in;
- login opt-in: `CT_SERVER_FLAGS` ORs in `SRVCAP_NATTRAVERSAL` (`ServerConnect.cpp`);
- callback-active state for the session: `CServer::IsLugdunumNatCallbackActive()`, set only
  when one `OP_IDCHANGE` both comes in the 24-byte form and carries the `0x1000` TCP flag
  (`LugdunumNatTraversal::IsNatCallbackActive()`). `CServer::SupportsLugdunumNatCallback()`
  still reports the flag alone, which proves nothing by itself (see above);
- keeping LowID sources from a callback-active server: `CPartFile::AddSources()` passes
  `keepLowIdSource` to `CanAddSource()`, which otherwise drops every LowID source while we are
  LowID. As in NeoMule (`CanLow2Low()`), an opted-in server sends only LowID sources it can
  call back, and source records carry no NatT flag of their own
  (`LugdunumNatTraversal::ShouldKeepLowIdServerSource()`);
- sending `OP_NAT_CALLBACKREQUEST` (`0xA6`): when we are LowID, the source we want is an odd
  LowID on our current, callback-active server (both server families give callback-capable
  clients odd LowIDs, so an even one cannot answer; the reverse callback follows the same rule),
  and the server-way UDP crypto preconditions hold
  (`thePrefs::IsServerCryptLayerUDPEnabled() && GetServerKeyUDP() != 0 &&
  SupportsObfuscationUDP()`), `BaseClient.cpp` sends one request from the client UDP socket,
  encrypted "the server way" (`CEncryptedDatagramSocket::EncryptSendServer`), to the server's
  `GetObfuscationPortUDP()`. It is hooked at the LowID-to-LowID decline in `TryToContact()`, so
  it is sent at most once per source: a `DS_LOWTOLOWIP` source is not asked again. Requests are
  not paced across sources; NeoMule does not pace them either (each waits 3 s for a ping first);
- receiving `0xA7` server-way on the client UDP socket: `ClientUDPSocket.cpp` tries the same key selection as `CServerUDPSocket::OnPacketReceived` (a pending crypt-ping challenge before the established server key) and dispatches only after successful decryption; `0x37` on the server TCP link uses the same callback action. With the runtime opt-in and uTP build gate, either opcode starts a temporary dial to the observed IP/UDP port (QUIC when the forward carries a user hash and [Sender behavior](#sender-behavior) allows it, otherwise uTP). The requester ID is a routing hint; a forwarded hash is the expected identity for CAPS/QUIC, while `OP_HELLO` still decides which source the stream attaches to. A zero requester ID still permits this dial but forbids another callback request after failure;
- after the stream's `OP_HELLO` identifies a known source, `ClientList.cpp` attaches the temporary socket to that verified peer. A SYN-time winner token is one-shot and authorizes the incoming socket only for the peer whose outgoing attempt it matched;
- incoming-SYN arbitration in `UtpStreamAcceptor.cpp` compares the observed remote IPv4/port with the local public IPv4 and the best available local UDP port: verified external Kad UDP port when known, otherwise configured UDP port, otherwise TCP port. The smaller endpoint tuple keeps its outgoing connection and rejects the incoming socket; the larger tuple keeps incoming. If endpoints are unavailable or equal, compare hashes only when both are valid; otherwise keep outgoing. A local outgoing-client token identifies the candidate through the dial and adapter but is never peer identity. The accepted winner carries that token until verified `OP_HELLO`; only then is the matching outgoing attempt retired, outside the libutp callback. If HELLO cannot be matched to the candidate, retain the outgoing attempt and apply the ordinary fail-closed source-socket policy. When an incoming winner replaces a connected download socket, its lost file request is sent again once after connection establishment;
- callback actions share a server-scoped guard across TCP `0x37` and UDP `0xA7`: an identical routing tuple is suppressed for 10 seconds (up to 256 entries), and at most 20 callback dials start per minute. Failed callback dials are separately remembered for 10 minutes by server, requester (endpoint-only for requester ID zero), and observed endpoint. A changed endpoint can retry immediately after the duplicate window, and a successful verified HELLO clears the matching failure record; the source remains eligible for a later callback after cooldown;
- NAT source-port remapping is limited to one live expected outgoing callback/punch peer on the same IPv4 address and within ±512 ports, with the existing 30-second expectation lifetime. The adapter gives upstream libutp the original handshake tuple and redirects only that socket's immediate response to the observed source port; it updates adapter transport state only after the frame is claimed. Incoming SYNs keep normal admission so the tie-break still runs, and established traffic remains bound to its exact tuple;
- pure callback codecs and shared failure policy live in `src/LugdunumNatTraversal.h`; endpoint, candidate, tie-break, and attachment decisions live in `src/UtpDialPolicy.h`.

The runtime setting and `ENABLE_NATT_SERVER_COORDINATION` remain off by default. This experimental behavior has deterministic policy and adapter coverage but has not been live-network validated.

**Live validation** (PR #2002, got3nks): on eMule Sunrise (`17.15 (lugdunum)`), an opted-in
LowID sharing a file and an opted-in LowID requester on the same server, both behind one public
IP, completed the opt-in and the server handed the requester a `DS_LOWTOLOWIP` source -- a
NatT-capable LowID source is only handed to a NatT-capable LowID requester, matching the wiki.
The requester's `OP_NAT_CALLBACKREQUEST` then reached the sharer: the forwarded callback arrived
as **`OP_NAT_CALLBACKREQUESTED` (`0x37`) over the TCP link**, not `0xA7` over UDP. This historical
observation confirms the relay delivery path; it does not validate the current uTP implementation.

## Statistics

The statistics tree shows, per session, how often the server-coordinated paths start a
LowID-to-LowID session and how much data such sessions carry. The nodes travel in the
tree like any other, so amulegui, amuleweb and amuleapi show them without new EC tags; each has a
stable `key`.

- **NAT Traversal (Session)** (`nat_traversal`, under Connection), in builds with
  `ENABLE_NATT_SERVER_COORDINATION` and uTP: attempts and successes for the ed2k-server punch
  (`natt_server_punch_attempts`, `natt_server_punch_successes`) and Lugdunum's callback
  (`natt_lugdunum_callback_attempts`, `natt_lugdunum_callback_successes`). An attempt is each
  rendezvous we act on, counted once per peer: a `0x61` we punch for, or a Lugdunum callback we
  dial. The extension counts under the ed2k-server punch, and its 5 s fallback dial under
  Lugdunum, as does a cross-server request: on the requester the attempt is the request and the
  success that source's hello, on the target the wake and the requester's hello. A success is a
  stream to or from that peer, on either side, whose hello is processed within 60 s
  (`NatServerHolePunch::RendezvousOutcomes`, at most 64 pending).
- **LowID to LowID Data (Session)** (`upload_lowid_to_lowid_data`,
  `download_lowid_to_lowid_data`), in the same builds: payload exchanged with a LowID peer while
  we are LowID on a server, which only NAT traversal connects.
- **IPv6 Data (Session)** (`upload_ipv6_data`, `download_ipv6_data`), in `ENABLE_IPV6` builds:
  payload exchanged with a peer connected over IPv6.

A session is classified when its hello is processed, on the main thread; the upload disk I/O
thread only reads the result.
