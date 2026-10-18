# eD2k server login and source-exchange IPv6 extension

This document specifies aMule's experimental support for the eD2k server-side
IPv6 extension agreed between eMuleQt, eNode-go and this project: advertising a
client's IPv6 address at login, and receiving IPv6 sources in
`OP_FOUNDSOURCES` / `OP_FOUNDSOURCES_OBFU`. It does not cover connecting to a
server over an IPv6 transport, which is out of scope, nor `OP_SERVERIDENT`'s
`ST_IPV6` / `ST_IPV6_STATUS` tags, which this implementation does not read.

## Availability

The exchange is compile-time gated by `ENABLE_IPV6`. A build without it sends
and parses exactly what it always has: four login tags, no `CT_MOD_IP_V6`, no
`CT_SERVER_FLAGS` `0x1000` bit, and `OP_FOUNDSOURCES` / `_OBFU` source ids are
never treated as anything but an ordinary userid.

## Login: advertising our IPv6 address

A client that currently advertises an IPv6 address to peers (`AdvertiseIPv6=1`
and the gates in `AdvertisedIPv6ForPeers()`: corroboration, an inbound-capable
listener, and no proxy) sends two additional things in `OP_LOGINREQUEST`, both
or neither:

- a `CT_MOD_IP_V6` (`0xAE`) tag, `TAGTYPE_HASH` (0x01), carrying the address as
  16 raw bytes -- the same tag and encoding the hello packet already sends to
  peers (`src/BaseClient.cpp`, `SendHelloTypePacket`);
- the `0x1000` bit set in the existing `CT_SERVER_FLAGS` (`0x20`) tag, meaning
  "this client can parse an IPv6 source record".

The login tag count is adjusted by exactly one (`CT_MOD_IP_V6`); the `0x1000`
bit is carried inside the already-counted `CT_SERVER_FLAGS` tag. The decision
is made once, in `DecideServerLoginIPv6()` (`src/ServerLoginIPv6.h`), so the
tag and the bit cannot drift apart. The address this login advertised is
remembered per server connection (`CServerSocket::GetLoginIPv6()`), because
it is also the condition under which a sentinel source record (below) is
expected, and it identifies our own record.

## Server flag: `0x4000`

A server that speaks this extension sets bit `0x4000` in the TCP flags word of
its `OP_IDCHANGE` reply (`src/include/protocol/ed2k/Client2Server/TCP.h`,
`SRV_TCPFLG_IPV6`). aMule recognises and logs it
(`CServer::SupportsIPv6Sources()`); it is not currently acted on beyond that.
This bit is deliberately not the same value as the client's `0x1000` login
flag -- the two name different directions of the same capability and must
never be unified into one bit.

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
and the `0x1000` bit at login; without that, `0xFFFFFFFF` is parsed as an
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

For a requester that advertised IPv6, ed2k-server sends a LowID source that
has a publishable IPv6 only as a sentinel, never as its LowID. Without this
extension, a HighID aMule reaches such a source through a server callback;
with it, only by dialling the IPv6. eMuleAI sends `CT_MOD_IP_V6` whenever it
holds a public IPv6, whether or not it accepts inbound IPv6, and many home
routers block inbound IPv6. So `AdvertiseIPv6=1` gains sources while we are
LowID, and can lose some while we are HighID. A server-side change to send the
sentinel only to LowID requesters is proposed in
[andrey23127/ed2k-server#33](https://github.com/andrey23127/ed2k-server/issues/33).

## Scope

Out of scope: connecting to a server over an IPv6 transport; `OP_SERVERIDENT`
`ST_IPV6` / `ST_IPV6_STATUS`; acting on the server's `0x4000` bit beyond
logging it; any change to the UDP ping/search/source path, which stays IPv4
only.

Limit: the server keeps the address from our login until the next login. If
our advertised IPv6 changes, or advertising stops (a corroboration veto or
the opt-out), the server keeps publishing the old address for the rest of the
session. An in-session update is proposed in
[andrey23127/ed2k-server#34](https://github.com/andrey23127/ed2k-server/issues/34).
