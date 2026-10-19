# eD2k server login and source-exchange IPv6 extension

This document specifies aMule's experimental support for the eD2k server-side
IPv6 extension agreed between eMuleQt, eNode-go and this project: advertising a
client's IPv6 address at login, and receiving IPv6 sources in
`OP_FOUNDSOURCES` / `OP_FOUNDSOURCES_OBFU`. It does not cover connecting to a
server over an IPv6 transport, which is out of scope, nor `OP_SERVERIDENT`'s
`ST_IPV6` / `ST_IPV6_STATUS` tags, which this implementation does not read.

## Availability

The exchange is compile-time gated by `ENABLE_IPV6`. A build without it sends
and parses exactly what it always has: four login tags, no `CT_MOD_IP_V6`, and
`OP_FOUNDSOURCES` / `_OBFU` source ids are never treated as anything but an
ordinary userid.

## Login: advertising our IPv6 address

A client that currently advertises an IPv6 address to peers (`AdvertiseIPv6=1`
and the gates in `AdvertisedIPv6ForPeers()`: corroboration, an inbound-capable
listener, and no proxy) adds a `CT_MOD_IP_V6` (`0xAE`) tag to
`OP_LOGINREQUEST`: `TAGTYPE_HASH` (0x01), carrying the address as 16 raw bytes
-- the same tag and encoding the hello packet already sends to peers
(`src/BaseClient.cpp`, `SendHelloTypePacket`). The tag alone tells the server
that we parse IPv6 source records; `ed2k-server` keys on nothing else. The
decision is made in `DecideServerLoginIPv6()` (`src/ServerLoginIPv6.h`). The
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
routers block inbound IPv6. So `AdvertiseIPv6=1` gains sources while we are
LowID, and can lose some while we are HighID. `ed2k-server` 0.9.83 fixed this:
it now sends the sentinel only to a LowID requester
(`ipv6_capable && !is_high_id`).

## Keeping the server current: `OP_IPV6_UPDATE`

On a connection where the server set `0x4000` and `0x8000`, aMule sends `OP_IPV6_UPDATE`
(opcode `0x63`): 16 raw bytes, all zeros to withdraw, no reply, same rules as
the login tag. It is sent whenever `AdvertisedIPv6ForPeers()` changes, stops,
or starts after login -- the same change detection that drives
`OP_CHANGE_CLIENT_IP` to peers (`src/amule.cpp`) -- and also right after an
`OP_IDCHANGE` carrying both bits, in case the address changed between the
login and that reply. The decision is `DecideServerIPv6Update()`
(`src/ServerIPv6Update.h`); the send path is
`CServerConnect::UpdateAdvertisedIPv6()`, which keeps
`CServerSocket::GetLoginIPv6()` -- the own-record check address -- in step
with whatever was actually sent.

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

## Scope

Out of scope: connecting to a server over an IPv6 transport; `OP_SERVERIDENT`
`ST_IPV6` / `ST_IPV6_STATUS`; any change to the UDP ping/search/source path,
which stays IPv4 only.
