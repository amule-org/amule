# eD2k server login and source-exchange IPv6 extension

This document specifies aMule's experimental support for the eD2k server-side
IPv6 extension agreed between eMuleQt, eNode-go and this project: advertising a
client's IPv6 address at login, and receiving IPv6 sources in
`OP_FOUNDSOURCES` / `OP_FOUNDSOURCES_OBFU`. It also covers the IPv6 server
callback, an ed2k-server proposal (below). It does not cover connecting to a
server over an IPv6 transport, which is out of scope, nor `OP_SERVERIDENT`'s
`ST_IPV6` tag, which this implementation does not read. It reads
`ST_IPV6_STATUS`, the probe verdict (below).

## Availability

The exchange is compile-time gated by `ENABLE_IPV6`. A build without it sends
and parses exactly what it always has: four login tags, no `CT_MOD_IP_V6`, and
`OP_FOUNDSOURCES` / `_OBFU` source ids are never treated as anything but an
ordinary userid.

## The dial-back probe and its verdict

A server that sets `0x400000` in `OP_IDCHANGE` dials the address before publishing it, and reports
what it found in `ST_IPV6_STATUS` (`0xAB`), a byte of bits: `0x01` address held, `0x02` reachable,
`0x04` probed. So `0x07` means the server reached us, `0x05` means it did not, `0x01` means the
probe is still running, and `0x03` is an address the server published on trust, which proves
nothing. aMule reads the tag from every `OP_SERVERIDENT`, because a later one carries it.

With `AdvertiseIPv6Mode` set to `0` (when verified), the login carries no address until a probe or
an inbound hello has proved it. In practice the probe is what can prove it: a peer can only dial
this address after learning it from our hello, Kad or a source list, and all of those wait for the
same decision, so the inbound hello can confirm an address that a probing server has already
checked, or one proved in the last 24 h, but it cannot start the process.
A server that does not announce the probe receives what the peers decision allows, so an address
already proved, such as one confirmed by an inbound hello in the last 24 h, still reaches it;
announcing the probe is what gets a server the chosen local address, through `OP_IPV6_UPDATE`,
once per real change: it needs the address to test it, and it does not publish anything it has not
reached. The login cannot carry that address, so a probing server also needs `0x4000` and
`0x8000` (below), or the address never reaches it. The server caps dial-back attempts, not
updates: four per session, the login's own address included.

With the crypt layer required, the probe must be obfuscated. aMule takes a plain incoming connection
only from a server it is still logging in to, matched by IPv4 address
(`CServerConnect::AwaitingTestFromIP()`), and in strict mode not even then. So it refuses a plain
probe from the server's IPv6 address, and the verdict is `0x05`. A server reaches such a client
only if it dials obfuscated, keyed with the user hash from the login. ed2k-server does this first
when the login's `CT_SERVER_FLAGS` say the client supports the crypt layer.

## Login: advertising our IPv6 address

A client that currently advertises an IPv6 address to peers (`AdvertiseIPv6Mode`
set to `1`, or to `0` with reachability verified,
and the gates in `AdvertisedIPv6ForPeers()`: corroboration, an inbound-capable
listener, and no proxy) adds a `CT_MOD_IP_V6` (`0xAE`) tag to
`OP_LOGINREQUEST`: `TAGTYPE_HASH` (0x01), carrying the address as 16 raw bytes
-- the same tag and encoding the hello packet already sends to peers
(`src/BaseClient.cpp`, `SendHelloTypePacket`). The tag alone tells the server
that we parse IPv6 source records; `ed2k-server` keys on nothing else. The
decision is made in `DecideServerLoginIPv6()` (`src/ServerLoginIPv6.h`). The
login comes before `OP_IDCHANGE`, so it cannot know whether the server probes:
it carries only what peers may see, and a probing server gets the address it
has to test right after `OP_IDCHANGE`, in `OP_IPV6_UPDATE` (below). The
address this login advertised is remembered per server connection
(`CServerSocket::GetLoginIPv6()`), because it is also the condition under
which a sentinel source record (below) is expected, and it identifies our own
record.

No `CT_SERVER_FLAGS` bit is set. `0x1000`, once specified for this, is
Lugdunum's NAT callback capability: Lugdunum answers a client that sets it
with an odd LowID, a longer `OP_IDCHANGE` and callback notices, and NeoLoader
reads it the same way. emule-qt withdrew it from the IPv6 extension for that
reason.

## Server flags: `0x4000` and `0x8000`

A server that speaks this extension sets bit `0x4000` in the TCP flags word of
its `OP_IDCHANGE` reply (`src/include/protocol/ed2k/Client2Server/TCP.h`,
`SRV_TCPFLG_IPV6`). aMule logs it (`CServer::SupportsIPv6Sources()`), and
requires it before trusting `0x8000` (below).

A server that also sets bit `0x8000` (`SRV_TCPFLG_IPV6_UPDATE`) additionally
accepts `OP_IPV6_UPDATE` on this connection -- see the next section. `0x8000`
is trusted only together with `0x4000`, because elsewhere it is read as a NAT
rendezvous flag (`ServerAcceptsIPv6Updates()` in `src/ServerIPv6Update.h`).

## Source records: the IPv6 sentinel

In `OP_FOUNDSOURCES` and `OP_FOUNDSOURCES_OBFU`, a source record whose id is
`0xFFFFFFFF` carries an IPv6 address instead of an IPv4 one. The address is
always the last field of the record, after any obfuscation fields, which is
what keeps the record self-delimiting:

```text
<id 4><port 2>                                   -- OP_FOUNDSOURCES
<id 4><port 2><crypt options 1>[<user hash 16>]  -- OP_FOUNDSOURCES_OBFU
                                 <ipv6 address 16> -- appended when id == 0xFFFFFFFF
```

A server only sends this sentinel to a client that advertised `CT_MOD_IP_V6`
at login; without that, `0xFFFFFFFF` is parsed as an
ordinary (and unusable, since `255.255.255.255` is never a real peer) userid,
exactly as before this extension existed. Per the upstream reference
(`ed2k-server`), IPv6 records are published only for sources that are LowID on
IPv4; aMule's server UDP path is IPv4-only, so it cannot carry this sentinel at
all (`CPartFile::AddSources()`'s `loginIPv6` parameter is null there).

The record parser is `ParseServerSourceRecord()`
(`src/ServerSourceRecords.h`), a pure function over the raw payload bytes that
returns either a parsed IPv4 record, a parsed IPv6 record, or a parse failure
for a truncated record. `CPartFile::AddSources()` drives it one record at a
time and keeps the existing IPv4 handling unchanged.

A parsed IPv6 source is added the same way `CDownloadQueue::KademliaSearchFile`
adds its Kad IPv6 source: no IPv4 address, dialled on the advertised address
alone (`CUpDownClient::SetUserAddress`), through the same gates every native
IPv6 contact goes through (`PeerAddressing::IsUsableTagIdentity`,
`DecideContactRoute`, the address-family policy, the proxy check and the IP
filter -- see `ShouldContactServerSourceOverIPv6()` in
`src/ServerIPv6SourcePolicy.h`). Unlike a Kad source, it is dialled whatever
our own client ID is, because the record carries no LowID to call back. A
sentinel naming our own advertised address and TCP port is skipped: a server
need not drop the requester's own record, and a plain `OP_FOUNDSOURCES`
carries no user hash to recognise it by.

## Trade-off for a HighID client

This trade-off applies only to an `ed2k-server` older than 0.9.83. For a
requester that advertised IPv6, such a server sends a LowID source that has a
publishable IPv6 only as a sentinel, never as its LowID. Without this
extension, a HighID aMule reaches such a source through a server callback;
with it, only by dialling the IPv6. eMuleAI sends `CT_MOD_IP_V6` whenever it
holds a public IPv6, whether or not it accepts inbound IPv6, and many home
routers block inbound IPv6. So advertising our address gains sources while we are
LowID, and can lose some while we are HighID. `ed2k-server` 0.9.83 fixed this:
it now sends the sentinel only to a LowID requester
(`ipv6_capable && !is_high_id`).

## Keeping the server current: `OP_IPV6_UPDATE`

On a connection where the server set `0x4000` and `0x8000`, aMule sends `OP_IPV6_UPDATE`
(opcode `0x63`): 16 raw bytes, all zeros to withdraw, no reply. It carries
`ServerAddressToSend()` (`src/AdvertisedIPv6.h`): for a server that sets
`0x400000`, the chosen local address for it to test; for any other, what peers
are told. It is sent right after an `OP_IDCHANGE` carrying both bits, and from
`AdvertisedIPv6ForPeers()` (`src/amule.cpp`), which the interface refresh,
hellos and Kad call, each time only when that address differs from the last one
sent. The decision is `DecideServerIPv6Update()` (`src/ServerIPv6Update.h`);
the send path is
`CServerConnect::UpdateAdvertisedIPv6()`, which keeps
`CServerSocket::GetLoginIPv6()` -- the own-record check address -- in step
with whatever was actually sent.

One repeat is sent on purpose. A pinhole that opens after a failed dial-back
(`0x05`) helps only once the server dials again, and `ed2k-server` 0.9.90 and
later dial a repeat of a failed address. So after a gateway grants a UPnP IGD2
pinhole, aMule sends the same address once more if the server's verdict on it
failed (`ShouldRepeatIPv6ForReprobe()`, same header). The check runs again when
an `OP_SERVERIDENT` brings a new verdict, for a pinhole that opened during the
first dial. A repeat while a dial runs, or after one passed, starts nothing, so
aMule does not send one then. Each repeat uses one of the server's four dials.

Because the server sends no reply, an `OP_FOUNDSOURCES` built while we were
still capable can still arrive after a withdrawal. Sentinel parsing is
therefore sticky for the rest of the server session once any IPv6 was
advertised on it, by the login or an update: it never turns back off, even
while `GetLoginIPv6()` itself goes back to Absent (`0xFFFFFFFF` is never a
real source id). `CServerSocket::IsIPv6SentinelParsingOn()` is the sticky
flag; `GetLoginIPv6()` is the separate, up-to-date own-record address. A
server that never sets both bits (any `ed2k-server` older than 0.9.83) never
receives an update, so it keeps publishing the address from our login until
the next login, even after it changed or advertising stopped.

## The IPv6 server callback

Two LowID clients on one server cannot reach each other over IPv4 without NAT traversal. When the
requester's IPv6 accepts inbound connections and the source's does not, the server can still join
them: it tells the source to dial the requester's IPv6. ed2k-server offers this. Its values are a
proposal still to be agreed with eNode-go.

- **Offer:** `OP_SERVERIDENT` carries `ST_IPV6_CALLBACK` (`0xAA`), an integer whose bit 0 says
  the server takes the request below. A server sends it only to a client that announced IPv6, and
  only while its dial-back runs. aMule reads it from every `OP_SERVERIDENT`, so its absence
  withdraws the offer.
- **Opt-in, as the source:** after an offer, aMule sends `OP_IPV6_CALLBACK_OPTIN` (`0x65`, no
  payload) once per server session. It does so only when it advertises an IPv6 to that server and
  can dial IPv6: the address family setting allows it, no proxy is set, and a local IPv6 is
  selected. The server sends the callback only to a client that opted in. Withdrawing the address
  with `OP_IPV6_UPDATE` also withdraws the opt-in, so aMule sends it again after the next address.
- **Request, as the requester:** while LowID, aMule asks for a LowID source of its own server with
  `OP_IPV6_CALLBACK_REQUEST` (`0x64`, `<target LowID 4>`). It asks only when the server's last
  `OP_SERVERIDENT` offered the callback and reported our IPv6 as reached by its dial-back
  (`ST_IPV6_STATUS` `0x07`): the server forwards no other address. The source then waits as
  "connecting via server".
- **Callback, as the source:** `OP_IPV6_CALLBACKREQUESTED` (`0x26`) is `<ipv6 16><port 2>`,
  followed by `<crypt options 1><user hash 16>` when 17 more bytes follow, as in
  `OP_CALLBACKREQUESTED`. aMule dials it over TCP after the checks it makes on an IPv6 source from
  a server record (a global address, not our own, the address family setting, no proxy) and the
  IP filter. eNode-go sends the short form, without an opt-in, to any client that announced IPv6.
  aMule answers it the same way.
- **Failure:** the server answers a request it does not forward with `OP_CALLBACK_FAIL` (`0x36`),
  `<target LowID 4><reason 1>`, as `OP_LOWID_HOLEPUNCH_FAIL` (`0x62`) does. aMule ends the
  request for that target, whatever else is out. A request with no answer ends after 10 s
  (`kIPv6CallbackAnswerMs` in `src/ServerIPv6Callback.h`). Either way the source goes on to the
  other LowID-to-LowID steps, the server hole punch and the Lugdunum callback, in builds that have
  them. A connection that comes later is still used. Some reasons also change what aMule asks
  next:
  - `1` (not offered), `2` (we are HighID) and `3` (our IPv6 is not verified): no request until
    the next `OP_SERVERIDENT` offers the callback again.
  - `6` (the source did not opt in): that source is not asked again in this server session.
  - `8` (over the relay allowance): the source is asked again after 30 s, not after the source
    reask time, or when a NAT-T step sent at the refusal has had its 60 s.
  - `4` (the source is us), `5` (the source is gone), `7` (the source is unreachable), and any
    other value end only that request.

  An empty `OP_CALLBACK_FAIL` answers `OP_CALLBACKREQUEST`. aMule takes it as a request's only
  while that request is the one out.
- **Sources:** while the callback can work (offered, and our IPv6 reached or still being tested),
  a LowID source of our own server is kept, not dropped as unreachable. A source that waits
  LowID-to-LowID is asked again at most once per source reask interval. So a source found before
  the dial-back's verdict gets the callback once the verdict arrives.

## Scope

Out of scope: connecting to a server over an IPv6 transport; `OP_SERVERIDENT`
`ST_IPV6`; any change to the UDP ping/search/source path, which stays IPv4
only.
