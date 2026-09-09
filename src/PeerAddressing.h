//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//
// Any parts of this program derived from the xMule, lMule or eMule project,
// or contributed by third-party developers are copyrighted by their
// respective authors.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA
//

#ifndef PEERADDRESSING_H
#define PEERADDRESSING_H

#include "NetworkAddress.h"

#include <cstdint>

/**
 * What identifies a peer, once a peer can be IPv6.
 *
 * These value policies prepare peer-address widening without changing production
 * call sites. Socket-ingress normalization belongs to the later call-sites PR;
 * local unmapping here defensively handles both native and mapped IPv4.
 *
 * Three separate questions live here, and they deliberately give different
 * answers for the same address:
 *
 *  - **Identity.** Two peers are the same peer iff they have the same index
 *    key. Aggregating distinct hosts here would merge them. Keeping absence
 *    separate from 0.0.0.0 is a prevention/hardening invariant, not a claim
 *    that #1314 shipped an unknown-address ban bug.
 *  - **Rate limiting.** A budget is per *subscriber*, which under IPv6 is not
 *    per address. Aggregating is the point here, and the amount of aggregation
 *    is a decision, not a detail.
 *  - **Routing an inbound datagram.** Some subsystems cannot represent an IPv6
 *    peer at all -- Kad by design, ed2k UDP obfuscation by protocol. Those
 *    boundaries are reported, never crossed with a fabricated address.
 *
 * Nothing here fabricates an address. Every function that cannot answer for an
 * address says so; an absent address stays absent all the way through.
 */
namespace PeerAddressing
{

/**
 * Whether a peer with this address can be recorded in an address index.
 *
 * Only absence disqualifies. @c 0.0.0.0 and @c :: are odd addresses but they are
 * addresses: a peer claiming one is a peer whose claim we know, which is not the
 * same thing as a peer whose address we do not know. Keeping those two apart is
 * the reason CNetworkAddress exists.
 */
inline bool IsIndexable(const CNetworkAddress &address) noexcept
{
	return address.IsPresent();
}

/**
 * The key a peer is indexed under.
 *
 * IPv4-mapped forms are collapsed to plain IPv4 here, once. A peer that
 * connects as @c 192.0.2.1 and later as @c ::ffff:192.0.2.1 is one peer over
 * one family, and giving it two identities would let it hold two queue slots,
 * two ban states and two credit records. CNetworkAddress deliberately does not
 * normalise on comparison -- that keeps its ordering total -- so the
 * normalisation is spelled out at the one place identity is decided.
 *
 * Everything else is returned unchanged, absence included: this never invents a
 * key for a peer that has no address.
 */
inline CNetworkAddress IndexKey(const CNetworkAddress &address)
{
	return address.Unmapped();
}

/**
 * Whether this peer can be named in an ed2k wire field or an on-disk record
 * that holds a 32-bit address.
 *
 * The ed2k protocol carries a peer's address as 32 bits: source exchange, the
 * server protocol, the relayed callback, the @c .part.met.seeds file. A native
 * IPv6 peer has no such form, so it cannot be published or persisted through
 * them -- and must be @b omitted rather than written as a zero, which would
 * publish "0.0.0.0" to every peer that asked for sources.
 *
 * Widening those formats is a protocol change and needs its own capability bit,
 * so this predicate is the boundary until one exists.
 */
inline bool HasEd2kWireForm(const CNetworkAddress &address) noexcept
{
	std::uint32_t unused = 0;
	return address.ToIPv4NetworkOrder(unused);
}

/**
 * Whether an inbound ed2k UDP datagram from this peer can be de-obfuscated.
 *
 * The ed2k UDP obfuscation key is MD5 over our user hash, a 32-bit address and
 * a magic byte -- see CEncryptedDatagramSocket::DecryptReceivedClient(), where
 * the receiver derives it from the sender's address, and EncryptSendClient(),
 * where the sender derives it from its own public IPv4. The protocol has no
 * IPv6 input to that key, so an obfuscated ed2k datagram from a native IPv6
 * peer is undecryptable by any implementation, not just by this one.
 *
 * Feeding the derivation a zero would produce a wrong key, the packet would
 * fail its magic-value check and be handled as junk, and nothing would record
 * why. So the boundary is reported here instead.
 */
inline bool SupportsEd2kUdpObfuscation(const CNetworkAddress &address) noexcept
{
	return address.IsIPv4() || address.IsIPv4Mapped();
}

/** How far an inbound datagram from a given peer can be routed. */
enum class EUdpRoute
{
	//! Not a usable endpoint: absent/unspecified address, or zero port.
	Reject,
	//! Full service. Everything ed2k and Kad can do for a 32-bit peer.
	Ed2kAndKad,
	//! ed2k only. Kad keeps its documented IPv4 interface.
	Ed2kOnly
};

/** A UDP peer's address and advertised or observed UDP port. */
struct UdpEndpoint
{
	CNetworkAddress address;
	std::uint16_t port = 0;
};

/**
 * Classifies an inbound datagram's endpoint for future call sites.
 *
 * This value policy is not wired into production handlers yet. Kad retains its
 * IPv4 conversion boundary; native IPv6 can only select the ed2k route.
 *
 * Reject does not distinguish absent from unspecified: the caller holds the
 * address and can say which in its log, exactly as CMuleUDPSocket already does.
 */
inline EUdpRoute ClassifyUdpPeer(const UdpEndpoint &endpoint) noexcept
{
	const CNetworkAddress address = endpoint.address.Unmapped();
	if (endpoint.port == 0 || address.IsAbsent() || address.IsUnspecified()) {
		return EUdpRoute::Reject;
	}
	return SupportsEd2kUdpObfuscation(address) ? EUdpRoute::Ed2kAndKad : EUdpRoute::Ed2kOnly;
}

/**
 * Whether a client's advertised UDP port names it as the sender of a datagram
 * that arrived from @p source, comparing both the address and UDP port.
 *
 * A peer advertises two ports and they are not the same number: the ed2k TCP
 * port it accepts connections on, and the UDP port it accepts datagrams on. A
 * lookup that identifies the sender of a datagram by the first of those matches
 * nothing at all in the field, and does so silently -- it looks exactly like
 * "we do not know this peer", which is also the honest answer for a stranger.
 * Naming the port dimension in one predicate is what keeps the two apart at the
 * call sites.
 *
 * Zero on either side is @b unknown, not a port, and never matches. A client we
 * know by address carries a zero UDP port when it never advertised one, and
 * treating that as a value to compare would make every such client a candidate
 * for a datagram whose source port is also zero. Behind a carrier NAT one
 * address is many peers, so that match would name an arbitrary one of them --
 * and the rendezvous relay vouches for whoever this lookup returns. It fails
 * closed instead, which costs nothing: a peer that advertised no UDP port could
 * not have been matched by an exact comparison either.
 */
inline bool MatchesUdpSource(const UdpEndpoint &advertised, const UdpEndpoint &source) noexcept
{
	return ClassifyUdpPeer(advertised) != EUdpRoute::Reject &&
	       ClassifyUdpPeer(source) != EUdpRoute::Reject && advertised.port == source.port &&
	       advertised.address.Unmapped() == source.address.Unmapped();
}

/**
 * How much of an IPv6 address a rate limit is counted against.
 *
 * A /64 is the smallest prefix an IPv6 subscriber is normally delegated, so it
 * is the smallest unit that behaves like "one customer". Larger aggregation
 * (/56, /48) would put unrelated subscribers of one provider in a single
 * bucket, where one of them could exhaust the budget for the others.
 * The eMuleQt /128 alternative is consciously rejected: rotating addresses
 * within a delegated /64 would evade per-host accounting. This /64 policy is
 * accounting only, never identity, index, ban or routing policy.
 */
constexpr unsigned kIPv6RateLimitPrefixBits = 64;

/**
 * The address a per-peer rate limit is counted against.
 *
 * IPv4 counts per address, which is what the 32-bit throttles did, so an IPv4
 * peer's budget is unchanged. IPv6 counts per /64.
 *
 * That asymmetry is the whole decision. An IPv4 address is roughly a host, so
 * per-address is per-host. An IPv6 /128 is not: a subscriber delegated a /64
 * can source every request from a fresh address, so a per-/128 limit counts to
 * one forever and throttles nothing at all. Applying the IPv4 shape unchanged
 * would therefore have been the same as removing the limit for IPv6.
 *
 * A mapped IPv4 address shares the IPv4 budget -- a peer must not double its
 * allowance by respelling its address -- and absence has no budget, because it
 * identifies nobody.
 */
inline CNetworkAddress RateLimitScope(const CNetworkAddress &address)
{
	const CNetworkAddress unmapped = address.Unmapped();
	if (!unmapped.IsIPv6()) {
		// IPv4, or absent. Either way this is already the scope.
		return unmapped;
	}
	return unmapped.TruncatedToPrefix(kIPv6RateLimitPrefixBits);
}

/**
 * Whether this address alone establishes that the peer accepts inbound
 * connections.
 *
 * Answers only for native IPv6, and only for globally routable addresses.
 *
 * LowID is an IPv4 concept: it is inferred from the ed2k ID a server issued,
 * and it means "behind something that will not accept an inbound connection".
 * An IPv6 peer has no ed2k ID, so the ID carries no information about it -- and
 * a peer whose ID field is zero would otherwise be read as firewalled and sent
 * down the callback path, which for an IPv6 peer cannot work: the callback goes
 * through an ed2k server or a Kad buddy, both of which speak 32-bit addresses.
 *
 * A link-local, unique-local, loopback or unspecified address proves nothing --
 * aMule cannot dial it from here -- so those keep the existing rule rather than
 * overriding it.
 */
inline bool IsDirectlyReachable(const CNetworkAddress &address) noexcept
{
	return address.IsGloballyRoutableIPv6();
}

} // namespace PeerAddressing

#endif // PEERADDRESSING_H
// File_checked_for_headers
