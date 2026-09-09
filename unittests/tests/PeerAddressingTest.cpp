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

// Peer identity: what identifies a peer once a peer can be IPv6.
//
// Before this change a peer was a 32-bit address, so an inbound IPv6 peer was
// accepted by the socket and then dropped -- there was nothing to index it
// under. Widening that identity has three failure modes worth pinning, and only
// one of them is about IPv6:
//
//   1. Merging two peers that are not the same peer. The exact bug this guards
//      against already happened once in this tree: the banned-client list
//      accepted an absent address, banned it as the key 0.0.0.0, and every
//      client with an unknown address then read back as banned. Any new
//      address-keyed container can recreate it.
//   2. Silently changing IPv4 behaviour. The characterisation half of this file
//      records what an IPv4 peer's identity does today, at the value level the
//      unit tests can reach, so a regression in the widening is loud.
//   3. Handing a subsystem an address it cannot represent. Kad is IPv4 behind a
//      documented conversion boundary, and ed2k UDP obfuscation derives its key
//      from a 32-bit address, so an inbound datagram has to be routed by what
//      its peer address can actually reach.

#include <muleunit/test.h>

#include <PeerAddressing.h>

#include <map>

using namespace muleunit;
using namespace PeerAddressing;

DECLARE_SIMPLE(PeerAddressing)

// ---------------------------------------------------------------------------
// Indexability
// ---------------------------------------------------------------------------

TEST(PeerAddressing, AbsentIsNeverIndexable)
{
	// The whole point of the type: absence is not an address, so it is not a
	// group in any index. 0.0.0.0 and :: are addresses -- odd ones, but a peer
	// claiming one is a peer, not an unknown.
	ASSERT_FALSE(IsIndexable(CNetworkAddress::Absent()));
	ASSERT_TRUE(IsIndexable(CNetworkAddress::FromString("0.0.0.0")));
	ASSERT_TRUE(IsIndexable(CNetworkAddress::FromString("::")));
	ASSERT_TRUE(IsIndexable(CNetworkAddress::FromString("192.0.2.1")));
	ASSERT_TRUE(IsIndexable(CNetworkAddress::FromString("2001:db8::1")));
}

TEST(PeerAddressing, AbsentAndAllZeroAreDifferentKeys)
{
	// This is the regression that already shipped once, in a different map. An
	// index keyed on the address type must not let the two share a bucket, and
	// the "absent" one must not have a bucket at all.
	std::multimap<CNetworkAddress, int> index;

	const CNetworkAddress absent = CNetworkAddress::Absent();
	const CNetworkAddress allZeroV4 = CNetworkAddress::FromString("0.0.0.0");
	const CNetworkAddress allZeroV6 = CNetworkAddress::FromString("::");

	ASSERT_TRUE(absent != allZeroV4);
	ASSERT_TRUE(absent != allZeroV6);
	ASSERT_TRUE(allZeroV4 != allZeroV6);

	index.insert(std::make_pair(allZeroV4, 1));
	index.insert(std::make_pair(allZeroV6, 2));

	ASSERT_EQUALS((size_t)1, index.count(allZeroV4));
	ASSERT_EQUALS((size_t)1, index.count(allZeroV6));
	// Nothing was recorded under absence, so nothing reads back under it --
	// which is what makes "unknown address" un-bannable rather than banned.
	ASSERT_EQUALS((size_t)0, index.count(absent));
}

TEST(PeerAddressing, MappedAndNativeIPv4AreTheSamePeer)
{
	// A blocked IPv4 peer reconnecting as ::ffff:a.b.c.d is the same peer, and
	// the index must not give it a second identity. The address type does not
	// normalise on comparison by design, so the index key is normalised
	// deliberately, once, here -- and the test says so rather than assuming it.
	const CNetworkAddress native = CNetworkAddress::FromString("192.0.2.1");
	const CNetworkAddress mapped = CNetworkAddress::FromString("::ffff:192.0.2.1");

	ASSERT_TRUE(native != mapped);
	ASSERT_TRUE(IndexKey(native) == IndexKey(mapped));
	ASSERT_TRUE(IndexKey(mapped).IsIPv4());

	// An absent address has no key, and asking for one does not invent one.
	ASSERT_TRUE(IndexKey(CNetworkAddress::Absent()).IsAbsent());

	// A native IPv6 address keys as itself, unchanged.
	const CNetworkAddress v6 = CNetworkAddress::FromString("2001:db8::1");
	ASSERT_TRUE(IndexKey(v6) == v6);
}

TEST(PeerAddressing, DistinctIPv6PeersNeverShareAKey)
{
	// Two peers in the same /64 are two peers. The rate-limit scope aggregates
	// them on purpose (below); the identity index must not.
	static const char *const addresses[] = {
		"2001:db8::1", "2001:db8::2", "2001:db8:0:0:1::1", "2001:db8:1::1", "fe80::1", "::1", "::"
	};
	static const size_t count = sizeof(addresses) / sizeof(addresses[0]);

	std::multimap<CNetworkAddress, int> index;
	for (size_t i = 0; i < count; ++i) {
		const CNetworkAddress address = CNetworkAddress::FromString(addresses[i]);
		ASSERT_TRUE(address.IsPresent());
		index.insert(std::make_pair(IndexKey(address), (int)i));
	}

	for (size_t i = 0; i < count; ++i) {
		const CNetworkAddress address = CNetworkAddress::FromString(addresses[i]);
		ASSERT_EQUALS((size_t)1, index.count(IndexKey(address)));
	}
	ASSERT_EQUALS(count, index.size());
}

// ---------------------------------------------------------------------------
// IPv4 characterisation -- must not change
// ---------------------------------------------------------------------------

TEST(PeerAddressing, IPv4RoundTripThroughIdentityIsExact)
{
	// A client's address used to be an ed2k-order uint32 and the accessors
	// still hand one out. Every IPv4 value must survive the wider storage
	// unchanged, or an IPv4 peer's identity, its wire encoding and its Kad
	// conversion all shift together and no interface changes.
	static const uint32_t values[] = {
		0x0100007Fu, 0x010200C0u, 0xFFFFFFFFu, 0x00000001u, 0x01000000u, 0x2A2A2A2Au
	};
	static const size_t count = sizeof(values) / sizeof(values[0]);

	for (size_t i = 0; i < count; ++i) {
		const CNetworkAddress address = CNetworkAddress::FromIPv4NetworkOrderOrAbsent(values[i]);
		ASSERT_TRUE(address.IsPresent());
		ASSERT_TRUE(IsIndexable(address));
		ASSERT_EQUALS(values[i], address.ToIPv4NetworkOrderOrZero());
		// And the identity key of an IPv4 peer is the IPv4 address itself:
		// grouping is byte-for-byte what the 32-bit multimap did.
		ASSERT_TRUE(IndexKey(address) == address);
	}

	// Zero still means "unknown" at the 32-bit boundary, and reads back as
	// zero on the way out. This is the pre-existing behaviour of a client with
	// no address, and it is unchanged by having somewhere wider to store one.
	const CNetworkAddress unknown = CNetworkAddress::FromIPv4NetworkOrderOrAbsent(0);
	ASSERT_TRUE(unknown.IsAbsent());
	ASSERT_FALSE(IsIndexable(unknown));
	ASSERT_EQUALS(0u, unknown.ToIPv4NetworkOrderOrZero());
}

TEST(PeerAddressing, NativeIPv6HasNoThirtyTwoBitForm)
{
	// The guarantee that stops a fabricated address reaching Kad, the ed2k wire
	// or an obfuscation key: the narrowing fails and leaves the caller's
	// variable alone.
	const CNetworkAddress v6 = CNetworkAddress::FromString("2001:db8::1");
	uint32_t scratch = 0xDEADBEEFu;
	ASSERT_FALSE(v6.ToIPv4NetworkOrder(scratch));
	ASSERT_EQUALS(0xDEADBEEFu, scratch);
	ASSERT_FALSE(v6.ToIPv4HostOrder(scratch));
	ASSERT_EQUALS(0xDEADBEEFu, scratch);
}

// ---------------------------------------------------------------------------
// Inbound datagram routing
// ---------------------------------------------------------------------------

TEST(PeerAddressing, DatagramRoutingTable)
{
	// No address at all, or one this build cannot parse: nothing to route to.
	ASSERT_TRUE(ClassifyUdpPeer(CNetworkAddress::Absent()) == EUdpRoute::Reject);

	// A peer claiming the unspecified address. Rejected, and distinguishable
	// from the absent case at the call site so the log can say which.
	ASSERT_TRUE(ClassifyUdpPeer(CNetworkAddress::FromString("0.0.0.0")) == EUdpRoute::Reject);
	ASSERT_TRUE(ClassifyUdpPeer(CNetworkAddress::FromString("::")) == EUdpRoute::Reject);

	// IPv4, and the mapped spelling of IPv4: everything is reachable.
	ASSERT_TRUE(ClassifyUdpPeer(CNetworkAddress::FromString("192.0.2.1")) == EUdpRoute::Ed2kAndKad);
	ASSERT_TRUE(
		ClassifyUdpPeer(CNetworkAddress::FromString("::ffff:192.0.2.1")) == EUdpRoute::Ed2kAndKad);

	// Native IPv6: the ed2k handlers can identify this peer now, Kad cannot.
	// Kad's 32-bit interface is a documented boundary, not an oversight, so the
	// route says so instead of narrowing and hoping.
	ASSERT_TRUE(ClassifyUdpPeer(CNetworkAddress::FromString("2001:db8::1")) == EUdpRoute::Ed2kOnly);
	ASSERT_TRUE(ClassifyUdpPeer(CNetworkAddress::FromString("fe80::1")) == EUdpRoute::Ed2kOnly);
}

TEST(PeerAddressing, Ed2kUdpObfuscationNeedsAThirtyTwoBitPeer)
{
	// The ed2k UDP obfuscation key is MD5 over our user hash, a 32-bit address
	// and a magic byte (EncryptedDatagramSocket.cpp). There is no IPv6 input to
	// that key in the protocol, so an obfuscated ed2k datagram from a native
	// IPv6 peer cannot be decrypted -- by anyone, not just by this build. The
	// boundary is reported rather than papered over with a zero, which would
	// derive a wrong key and read the packet as junk without saying why.
	ASSERT_TRUE(SupportsEd2kUdpObfuscation(CNetworkAddress::FromString("192.0.2.1")));
	ASSERT_TRUE(SupportsEd2kUdpObfuscation(CNetworkAddress::FromString("::ffff:192.0.2.1")));
	ASSERT_FALSE(SupportsEd2kUdpObfuscation(CNetworkAddress::FromString("2001:db8::1")));
	ASSERT_FALSE(SupportsEd2kUdpObfuscation(CNetworkAddress::Absent()));
}

// ---------------------------------------------------------------------------
// Rate-limit scope
// ---------------------------------------------------------------------------

TEST(PeerAddressing, IPv4RateLimitScopeIsTheAddress)
{
	// Unchanged from the 32-bit throttle: one address, one budget. Neighbours
	// are not aggregated, so a shared IPv4 address is the only thing that
	// shares a budget -- which is what it meant before.
	const CNetworkAddress a = CNetworkAddress::FromString("192.0.2.1");
	const CNetworkAddress b = CNetworkAddress::FromString("192.0.2.2");

	ASSERT_TRUE(RateLimitScope(a) == a);
	ASSERT_TRUE(RateLimitScope(a) != RateLimitScope(b));

	// The mapped spelling shares the IPv4 budget: it is the same host over the
	// same family, and a peer must not double its allowance by respelling.
	ASSERT_TRUE(RateLimitScope(CNetworkAddress::FromString("::ffff:192.0.2.1")) == a);

	// Absence has no budget, because it identifies nobody.
	ASSERT_TRUE(RateLimitScope(CNetworkAddress::Absent()).IsAbsent());
}

TEST(PeerAddressing, IPv6RateLimitScopeAggregatesAtSixtyFour)
{
	// The decision this change had to make. A /128 budget under IPv6 throttles
	// nothing: a subscriber holding a /64 sources each request from a fresh
	// address and every one of them looks like a first offence. Aggregating at
	// /64 restores the IPv4 meaning of the limit -- one subscriber, one budget.
	ASSERT_EQUALS(64u, kIPv6RateLimitPrefixBits);

	const CNetworkAddress first = CNetworkAddress::FromString("2001:db8:1:2::1");
	const CNetworkAddress second = CNetworkAddress::FromString("2001:db8:1:2:ffff:ffff:ffff:ffff");
	const CNetworkAddress neighbour = CNetworkAddress::FromString("2001:db8:1:3::1");

	// Same /64: one budget.
	ASSERT_TRUE(RateLimitScope(first) == RateLimitScope(second));
	// The scope is the prefix itself, host bits cleared.
	ASSERT_TRUE(RateLimitScope(first) == CNetworkAddress::FromString("2001:db8:1:2::"));

	// A different /64 is a different subscriber and keeps its own budget. This
	// is the other half of the decision: aggregating at /48 or /56 would put
	// unrelated customers of one provider in a single bucket, where one of them
	// could lock out the rest.
	ASSERT_TRUE(RateLimitScope(first) != RateLimitScope(neighbour));

	// An IPv6 scope never collides with an IPv4 one.
	ASSERT_TRUE(RateLimitScope(first) != RateLimitScope(CNetworkAddress::FromString("192.0.2.1")));
}

// ---------------------------------------------------------------------------
// Direct reachability
// ---------------------------------------------------------------------------

TEST(PeerAddressing, GlobalIPv6PeerIsDirectlyReachable)
{
	// LowID is an IPv4-with-NAT concept: it means a peer that cannot accept an
	// inbound connection, inferred from an ed2k ID a server issued. An IPv6
	// peer has no ed2k ID at all, so the ID says nothing about it, and a
	// globally routable IPv6 address is exactly the case where a callback is
	// neither needed nor possible.
	ASSERT_TRUE(IsDirectlyReachable(CNetworkAddress::FromString("2001:db8::1")));

	// Addresses aMule cannot dial from here, so they prove nothing about
	// reachability and must not suppress the callback path.
	ASSERT_FALSE(IsDirectlyReachable(CNetworkAddress::FromString("fe80::1")));
	ASSERT_FALSE(IsDirectlyReachable(CNetworkAddress::FromString("fc00::1")));
	ASSERT_FALSE(IsDirectlyReachable(CNetworkAddress::FromString("::1")));
	ASSERT_FALSE(IsDirectlyReachable(CNetworkAddress::FromString("::")));
	ASSERT_FALSE(IsDirectlyReachable(CNetworkAddress::Absent()));

	// IPv4, mapped or not, is left to the existing ed2k ID rule: this predicate
	// answers only for the family the ID cannot describe.
	ASSERT_FALSE(IsDirectlyReachable(CNetworkAddress::FromString("192.0.2.1")));
	ASSERT_FALSE(IsDirectlyReachable(CNetworkAddress::FromString("::ffff:192.0.2.1")));
}

TEST(PeerAddressing, RateLimitScopeMembersAreContiguousInTheIndexOrder)
{
	// CClientList counts a peer's slots by starting at its scope's network
	// address and walking while the scope holds. That is only correct if every
	// member of a prefix occupies one contiguous run of the ordering -- if it
	// did not, the walk would stop early and the limit would under-count, which
	// is a limit that silently does not apply. The ordering is octet-wise, most
	// significant first, so it does; asserted here because the scan depends on
	// it and the ordering lives in another file.
	std::multimap<CNetworkAddress, int> index;

	static const char *const addresses[] = { // Inside the /64 under test, deliberately out of order.
		"2001:db8:1:2:ffff:ffff:ffff:ffff",
		"2001:db8:1:2::",
		"2001:db8:1:2:8000::1",
		// Immediately outside it, on both sides.
		"2001:db8:1:1:ffff:ffff:ffff:ffff",
		"2001:db8:1:3::",
		// Far away, and in the other family.
		"2001:db8:9::1",
		"192.0.2.1",
		"255.255.255.255"
	};
	static const size_t count = sizeof(addresses) / sizeof(addresses[0]);
	for (size_t i = 0; i < count; ++i) {
		const CNetworkAddress address = CNetworkAddress::FromString(addresses[i]);
		ASSERT_TRUE(address.IsPresent());
		index.insert(std::make_pair(IndexKey(address), (int)i));
	}

	const CNetworkAddress scope = RateLimitScope(CNetworkAddress::FromString("2001:db8:1:2::5"));
	ASSERT_TRUE(scope == CNetworkAddress::FromString("2001:db8:1:2::"));

	size_t visited = 0;
	for (std::multimap<CNetworkAddress, int>::const_iterator it = index.lower_bound(scope);
		it != index.end();
		++it) {
		if (RateLimitScope(it->first) != scope) {
			break;
		}
		++visited;
	}
	// Exactly the three members, and the walk was not cut short by the
	// neighbouring /64 sorting in between them.
	ASSERT_EQUALS((size_t)3, visited);
}

TEST(PeerAddressing, Ed2kWireFormIsWhatMayBePublished)
{
	// Source exchange, the .part.met.seeds record and the relayed callback all
	// carry a peer as a 32-bit address. A native IPv6 peer has none, and the
	// only correct thing to do with it in those formats is to leave it out: a
	// zero there would publish 0.0.0.0 as a source to every peer that asked,
	// and persist it for the next start to dial.
	//
	// This matters more after the widening than before it. An IPv6 peer used to
	// be excluded from those paths incidentally, by having no ed2k id and so
	// reading as LowID. It is now correctly HighID, so the exclusion has to be
	// this explicit test instead.
	ASSERT_TRUE(HasEd2kWireForm(CNetworkAddress::FromString("192.0.2.1")));
	ASSERT_TRUE(HasEd2kWireForm(CNetworkAddress::FromString("0.0.0.0")));
	ASSERT_TRUE(HasEd2kWireForm(CNetworkAddress::FromString("::ffff:192.0.2.1")));
	ASSERT_FALSE(HasEd2kWireForm(CNetworkAddress::FromString("2001:db8::1")));
	ASSERT_FALSE(HasEd2kWireForm(CNetworkAddress::Absent()));

	// A peer that may be published is published with exactly the bytes it had
	// before the widening -- the mapped spelling included, which narrows to its
	// embedded IPv4 rather than to something new.
	uint32_t wire = 0;
	ASSERT_TRUE(CNetworkAddress::FromString("192.0.2.1").ToIPv4NetworkOrder(wire));
	ASSERT_EQUALS(0x010200C0u, wire);
	uint32_t mappedWire = 0;
	ASSERT_TRUE(CNetworkAddress::FromString("::ffff:192.0.2.1").ToIPv4NetworkOrder(mappedWire));
	ASSERT_EQUALS(wire, mappedWire);
}

// ---------------------------------------------------------------------------
// UDP source ports
// ---------------------------------------------------------------------------

TEST(PeerAddressing, UnknownUDPPortIdentifiesNobody)
{
	// A client we know by address has a UDP port only if it advertised one, and
	// zero is how "it never did" is spelled. Comparing that zero for equality
	// would make every such client a candidate for a datagram whose source port
	// is also zero -- and behind a carrier NAT one address is many peers, so
	// the match would name an arbitrary one of them. The rendezvous relay
	// vouches for whoever this lookup returns, so the unknown side has to lose.
	ASSERT_FALSE(MatchesUdpSourcePort(0, 0));
	ASSERT_FALSE(MatchesUdpSourcePort(0, 4672));
	ASSERT_FALSE(MatchesUdpSourcePort(4672, 0));
}

TEST(PeerAddressing, AdvertisedUDPPortMatchesOnlyItself)
{
	// The ordinary case, and the reason this predicate exists at all: the port
	// a peer advertised for UDP is not the ed2k TCP port it also advertised,
	// so a lookup that compares the wrong one of the two answers "unknown peer"
	// for every real peer while looking like it works.
	ASSERT_TRUE(MatchesUdpSourcePort(4672, 4672));
	ASSERT_FALSE(MatchesUdpSourcePort(4672, 4662));
	ASSERT_TRUE(MatchesUdpSourcePort(65535, 65535));
}

// File_checked_for_headers
