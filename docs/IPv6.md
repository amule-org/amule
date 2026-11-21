# IPv6

Native IPv6 is **experimental**. It needs a build with `ENABLE_IPV6` (off by default; see
[INSTALL.md](INSTALL.md)). A build without it uses IPv4 only. The server side, advertising our
address at login and receiving IPv6 sources, is in
[IPv6_Server_Sources.md](IPv6_Server_Sources.md).

## Listener and outbound dials

`AddressFamilies` selects the ed2k TCP listener: `1` is a dual-stack listener. It is in the
Connection preferences tab (aMule, amulegui and the Web UI), in amuleapi as
`connection.ip_versions`, or in `amule.conf`, and it takes effect only after a restart.

With `1`, aMule also dials native IPv6 peers. That includes Kad sources that are firewalled on
IPv4 and publish an IPv6 address (eMuleAI's `ip6` tag), when we are firewalled too.

## Advertising our address to peers

`AdvertiseIPv6Mode`, in the same tab (amuleapi: `connection.ipv6_advertising`), tells peers our
stable IPv6 address. It applies at once:

| Value | Meaning |
| --- | --- |
| `0` | When verified (the default) |
| `1` | Always |
| `2` | Never |

`1` is what the old `AdvertiseIPv6=1` meant. Choose it only if the ed2k TCP port accepts inbound
IPv6 connections, because eMuleAI peers then dial only that address.

In every mode, advertising stops for up to a day when several unrelated peers see us arriving from
a prefix this host does not hold (IPv6 NAT) and none has confirmed ours.

Connecting to a server over an IPv6 transport is out of scope.
