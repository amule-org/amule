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

// Contract of the internal address type: byte order stated in the signature, absence
// distinguishable from the all-zero address, failed narrowing rather than truncation, and a total
// order fit to key a container.
//
// This is the type every uint32 IP in the tree is being migrated onto, so its contract is pinned
// here rather than left to the applications. None of it needs a running aMule: the type is a value
// type over sixteen octets and a family tag.

#include <muleunit/test.h>

#include <NetworkAddressAsio.h>
#include <NetworkAddress.h>

#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>

using namespace muleunit;

DECLARE_SIMPLE(NetworkAddress)

// 192.0.2.1 (RFC 5737 documentation range) in each of the two conventions.
static const uint32_t TEST_IP_HOST_ORDER = 0xC0000201u;
static const uint32_t TEST_IP_ED2K_ORDER = 0x010200C0u;

// An IPv4-mapped address is IPv4 for every other accessor, so truncation has to use the effective
// family too. Taking 128 bits for it zeroed the embedded octets away entirely, which put every
// mapped peer into one bucket of the per-prefix budget this function exists to feed.
TEST(NetworkAddress, TruncatingAMappedAddressUsesTheIPv4Width)
{
	const CNetworkAddress mapped = CNetworkAddress::FromString("::ffff:203.0.113.5");
	ASSERT_TRUE(mapped.IsIPv4Mapped());

	const CNetworkAddress prefix = mapped.TruncatedToPrefix(24);
	ASSERT_FALSE(prefix.IsUnspecified());
	ASSERT_TRUE(prefix == CNetworkAddress::FromString("203.0.113.0"));

	// Two mapped peers in different /24s must not share a bucket.
	const CNetworkAddress other = CNetworkAddress::FromString("::ffff:198.51.100.5");
	ASSERT_FALSE(other.TruncatedToPrefix(24) == prefix);
}

// The scope is dropped on every path, not just the truncating one. Returning it only when
// prefixBits reached the family width made the same two addresses one value at /64 and two at
// /128, a discontinuity at the boundary that contradicts the function's own comment.
TEST(NetworkAddress, TruncationDropsTheScopeAtEveryWidth)
{
	const CNetworkAddress a = CNetworkAddress::FromString("fe80::1%7");
	const CNetworkAddress b = CNetworkAddress::FromString("fe80::1%9");
	if (a.GetScopeId() == b.GetScopeId()) {
		return; // The platform did not parse the scope suffixes; nothing to compare.
	}

	ASSERT_TRUE(a.TruncatedToPrefix(64) == b.TruncatedToPrefix(64));
	ASSERT_TRUE(a.TruncatedToPrefix(128) == b.TruncatedToPrefix(128));
}

// ::a.b.c.d, the deprecated IPv4-compatible form. Advertising one burns a peer's connect attempt,
// which is the cost this predicate exists to avoid.
TEST(NetworkAddress, IPv4CompatibleAddressesAreNotGloballyRoutable)
{
	ASSERT_FALSE(CNetworkAddress::FromString("::203.0.113.5").IsGloballyRoutableIPv6());
	// Still distinguished from the two values that share the same page.
	ASSERT_FALSE(CNetworkAddress::FromString("::").IsGloballyRoutableIPv6());
	ASSERT_FALSE(CNetworkAddress::FromString("::1").IsGloballyRoutableIPv6());
	// And a real address is unaffected.
	ASSERT_TRUE(CNetworkAddress::FromString("2001:4860:4860::8888").IsGloballyRoutableIPv6());
}

// The hash has to agree with operator==, which a byte-wise hash would not: sizeof is larger than
// the members and nothing initialises the tail padding.
TEST(NetworkAddress, HashingAgreesWithEquality)
{
	const CNetworkAddress a = CNetworkAddress::FromString("2001:db8::1");
	const CNetworkAddress b = CNetworkAddress::FromString("2001:db8::1");
	ASSERT_TRUE(a == b);
	ASSERT_EQUALS(std::hash<CNetworkAddress>()(a), std::hash<CNetworkAddress>()(b));

	std::unordered_map<CNetworkAddress, int> byAddress;
	byAddress[a] = 1;
	byAddress[b] = 2;
	ASSERT_EQUALS(1u, (unsigned)byAddress.size());
	ASSERT_EQUALS(2, byAddress[a]);

	// Distinct values, including the mapped and native forms the type keeps apart.
	byAddress[CNetworkAddress::FromString("203.0.113.5")] = 3;
	byAddress[CNetworkAddress::FromString("::ffff:203.0.113.5")] = 4;
	ASSERT_EQUALS(3u, (unsigned)byAddress.size());
}

TEST(NetworkAddress, ByteOrderIsInTheSignature)
{
	const CNetworkAddress fromHost = CNetworkAddress::FromIPv4HostOrder(TEST_IP_HOST_ORDER);
	const CNetworkAddress fromNetwork = CNetworkAddress::FromIPv4NetworkOrder(TEST_IP_ED2K_ORDER);

	// Both name the same address, reached through the convention each caller has.
	ASSERT_EQUALS(wxString("192.0.2.1"), fromHost.ToWxString());
	ASSERT_EQUALS(wxString("192.0.2.1"), fromNetwork.ToWxString());
	ASSERT_TRUE(fromHost == fromNetwork);

	// And they round-trip back into the convention that is asked for, not into
	// whichever one they were built from.
	uint32_t out = 0;
	ASSERT_TRUE(fromHost.ToIPv4HostOrder(out));
	ASSERT_EQUALS(TEST_IP_HOST_ORDER, out);
	ASSERT_TRUE(fromHost.ToIPv4NetworkOrder(out));
	ASSERT_EQUALS(TEST_IP_ED2K_ORDER, out);
	ASSERT_TRUE(fromNetwork.ToIPv4HostOrder(out));
	ASSERT_EQUALS(TEST_IP_HOST_ORDER, out);

	// The two conventions are byte reversals of each other, which is the
	// identity wxUINT32_SWAP_ALWAYS relies on at every existing swap site.
	ASSERT_EQUALS(TEST_IP_ED2K_ORDER, CNetworkAddress::SwapOctets(TEST_IP_HOST_ORDER));
	ASSERT_EQUALS(TEST_IP_HOST_ORDER, CNetworkAddress::SwapOctets(TEST_IP_ED2K_ORDER));

	// Feeding a value in the wrong convention cannot be a silent no-op: it yields a different,
	// visibly wrong address. Nothing stops a caller doing that, but nothing hides it either.
	ASSERT_EQUALS(
		wxString("1.2.0.192"), CNetworkAddress::FromIPv4HostOrder(TEST_IP_ED2K_ORDER).ToWxString());
}

TEST(NetworkAddress, TextualFormsPreserveUtf8)
{
	const CNetworkAddress ipv4 = CNetworkAddress::FromString("1.2.3.4");
	const CNetworkAddress ipv6 = CNetworkAddress::FromString("2001:db8::1");
	const CNetworkAddress absent = CNetworkAddress::Absent();

	ASSERT_EQUALS(wxString("1.2.3.4"), ipv4.ToWxString());
	ASSERT_EQUALS(wxString("2001:db8::1"), ipv6.ToWxString());
	ASSERT_EQUALS(wxString("<absent>"), absent.ToWxString());
	ASSERT_EQUALS(std::string("1.2.3.4"), ipv4.ToString());
}

TEST(NetworkAddress, MappedAndNativeAreDistinct)
{
	const CNetworkAddress native = CNetworkAddress::FromIPv4HostOrder(TEST_IP_HOST_ORDER);
	const CNetworkAddress mapped = CNetworkAddress::FromString("::ffff:192.0.2.1");

	ASSERT_TRUE(native.IsIPv4());
	ASSERT_FALSE(native.IsIPv6());
	ASSERT_FALSE(native.IsIPv4Mapped());

	ASSERT_TRUE(mapped.IsIPv6());
	ASSERT_FALSE(mapped.IsIPv4());
	ASSERT_TRUE(mapped.IsIPv4Mapped());

	// The distinction is preserved: the mapped form is an IPv6 address and is
	// not equal to the IPv4 address it embeds.
	ASSERT_TRUE(native != mapped);

	// A caller that wants them treated alike says so.
	ASSERT_TRUE(mapped.Unmapped() == native);
	ASSERT_TRUE(native.Unmapped() == native);

	// The mapped form still narrows losslessly, in either convention.
	uint32_t out = 0;
	ASSERT_TRUE(mapped.ToIPv4HostOrder(out));
	ASSERT_EQUALS(TEST_IP_HOST_ORDER, out);
	ASSERT_TRUE(mapped.ToIPv4NetworkOrder(out));
	ASSERT_EQUALS(TEST_IP_ED2K_ORDER, out);

	// A non-mapped v6 address is not mistaken for a mapped one, whichever end
	// of the ::ffff: prefix differs.
	ASSERT_FALSE(CNetworkAddress::FromString("2001:db8::1").IsIPv4Mapped());
	ASSERT_FALSE(CNetworkAddress::FromString("::fffe:192.0.2.1").IsIPv4Mapped());
	ASSERT_FALSE(CNetworkAddress::FromString("1::ffff:192.0.2.1").IsIPv4Mapped());
}

TEST(NetworkAddress, NarrowingAnIPv6AddressFails)
{
	const CNetworkAddress v6 = CNetworkAddress::FromString("2001:db8::1");
	ASSERT_TRUE(v6.IsPresent());
	ASSERT_TRUE(v6.IsIPv6());

	// The failure is reported, and the caller's variable is left exactly as it was -- no
	// truncation, no hash, no fabricated value even for a caller that ignores the return.
	uint32_t out = 0xDEADBEEFu;
	ASSERT_FALSE(v6.ToIPv4HostOrder(out));
	ASSERT_EQUALS(0xDEADBEEFu, out);
	ASSERT_FALSE(v6.ToIPv4NetworkOrder(out));
	ASSERT_EQUALS(0xDEADBEEFu, out);

	// The unspecified IPv6 address is not narrowable either, even though its
	// octets would truncate to a value that happens to look valid.
	const CNetworkAddress v6Any = CNetworkAddress::FromString("::");
	ASSERT_TRUE(v6Any.IsPresent());
	ASSERT_TRUE(v6Any.IsUnspecified());
	out = 0xDEADBEEFu;
	ASSERT_FALSE(v6Any.ToIPv4NetworkOrder(out));
	ASSERT_EQUALS(0xDEADBEEFu, out);

	// The lossy convenience form reports the same failure as a zero, which is
	// why it is only for edges that cannot act on the difference.
	ASSERT_EQUALS(0u, v6.ToIPv4NetworkOrderOrZero());
}

TEST(NetworkAddress, AbsenceIsNotTheAllZeroAddress)
{
	const CNetworkAddress absent;
	const CNetworkAddress zero = CNetworkAddress::FromIPv4NetworkOrder(0);

	ASSERT_TRUE(absent.IsAbsent());
	ASSERT_FALSE(absent.IsPresent());
	ASSERT_FALSE(absent.IsUnspecified()); // it is not an address at all
	ASSERT_TRUE(absent == CNetworkAddress::Absent());

	ASSERT_TRUE(zero.IsPresent());
	ASSERT_TRUE(zero.IsUnspecified());
	ASSERT_TRUE(zero.IsIPv4());
	ASSERT_EQUALS(wxString("0.0.0.0"), zero.ToWxString());

	// The whole point: these two are different values.
	ASSERT_TRUE(absent != zero);
	ASSERT_EQUALS(wxString("<absent>"), absent.ToWxString());

	// 0.0.0.0 narrows to zero; an absent address refuses to narrow at all, and
	// again leaves the caller's variable untouched.
	uint32_t out = 7;
	ASSERT_TRUE(zero.ToIPv4NetworkOrder(out));
	ASSERT_EQUALS(0u, out);
	out = 7;
	ASSERT_FALSE(absent.ToIPv4NetworkOrder(out));
	ASSERT_EQUALS(7u, out);

	// The boundary conversions resolve the ed2k zero overload towards absence,
	// and only there.
	ASSERT_TRUE(CNetworkAddress::FromIPv4NetworkOrderOrAbsent(0).IsAbsent());
	ASSERT_TRUE(CNetworkAddress::FromIPv4HostOrderOrAbsent(0).IsAbsent());
	ASSERT_TRUE(CNetworkAddress::FromIPv4NetworkOrder(0).IsPresent());
	ASSERT_TRUE(CNetworkAddress::FromIPv4HostOrder(0).IsPresent());
	ASSERT_TRUE(CNetworkAddress::FromIPv4NetworkOrderOrAbsent(TEST_IP_ED2K_ORDER) ==
		    CNetworkAddress::FromIPv4NetworkOrder(TEST_IP_ED2K_ORDER));

	// A string that is not an address is absent, not 0.0.0.0 -- unlike
	// StringIPtoUint32(), which cannot tell the caller the difference.
	ASSERT_TRUE(CNetworkAddress::FromString("").IsAbsent());
	ASSERT_TRUE(CNetworkAddress::FromString("not an address").IsAbsent());
	ASSERT_TRUE(CNetworkAddress::FromString("192.0.2.256").IsAbsent());
	ASSERT_TRUE(CNetworkAddress::FromString("0.0.0.0").IsPresent());
}

TEST(NetworkAddress, OrderingIsTotalAndUsableAsAKey)
{
	const CNetworkAddress absent;
	const CNetworkAddress v4Zero = CNetworkAddress::FromString("0.0.0.0");
	const CNetworkAddress v4Low = CNetworkAddress::FromString("10.0.0.1");
	const CNetworkAddress v4High = CNetworkAddress::FromString("192.0.2.1");
	const CNetworkAddress mapped = CNetworkAddress::FromString("::ffff:192.0.2.1");
	const CNetworkAddress v6 = CNetworkAddress::FromString("2001:db8::1");

	// Absent first, then IPv4 by value, then IPv6.
	ASSERT_TRUE(absent < v4Zero);
	ASSERT_TRUE(v4Zero < v4Low);
	ASSERT_TRUE(v4Low < v4High);
	ASSERT_TRUE(v4High < mapped);
	ASSERT_TRUE(mapped < v6);

	// Antisymmetry and irreflexivity, which is what makes it a strict weak
	// order and therefore safe for std::map.
	ASSERT_FALSE(v4High < v4High);
	ASSERT_FALSE(v4High < v4Low);
	ASSERT_TRUE(v4Low <= v4High);
	ASSERT_TRUE(v4High >= v4Low);
	ASSERT_TRUE(v4High > v4Low);
	ASSERT_FALSE(absent < CNetworkAddress::Absent());

	// Transitivity across the family boundary.
	ASSERT_TRUE(absent < v6);
	ASSERT_TRUE(v4Zero < v6);

	// No two distinct addresses collide, and equal ones do not duplicate.
	std::set<CNetworkAddress> keys;
	keys.insert(absent);
	keys.insert(v4Zero);
	keys.insert(v4Low);
	keys.insert(v4High);
	keys.insert(mapped);
	keys.insert(v6);
	ASSERT_EQUALS(6u, (unsigned)keys.size());
	keys.insert(CNetworkAddress::FromIPv4HostOrder(TEST_IP_HOST_ORDER)); // == v4High
	keys.insert(CNetworkAddress::Absent());
	ASSERT_EQUALS(6u, (unsigned)keys.size());

	// The order is the one documented, in order.
	std::set<CNetworkAddress>::const_iterator it = keys.begin();
	ASSERT_TRUE((it++)->IsAbsent());
	ASSERT_EQUALS(wxString("0.0.0.0"), (it++)->ToWxString());
	ASSERT_EQUALS(wxString("10.0.0.1"), (it++)->ToWxString());
	ASSERT_EQUALS(wxString("192.0.2.1"), (it++)->ToWxString());
	ASSERT_TRUE((it++)->IsIPv4Mapped());
	ASSERT_EQUALS(wxString("2001:db8::1"), (it++)->ToWxString());
	ASSERT_TRUE(it == keys.end());

	// Lookup by an equal address built the other way round still hits.
	std::map<CNetworkAddress, int> byAddress;
	byAddress[v4High] = 1;
	byAddress[mapped] = 2;
	ASSERT_EQUALS(2u, (unsigned)byAddress.size());
	ASSERT_EQUALS(1, byAddress[CNetworkAddress::FromIPv4NetworkOrder(TEST_IP_ED2K_ORDER)]);
	ASSERT_EQUALS(2, byAddress[CNetworkAddress::FromString("::ffff:c000:201")]);
	ASSERT_EQUALS(2u, (unsigned)byAddress.size());
}

TEST(NetworkAddress, TruncatedToPrefixClearsHostBits)
{
	// The prefix operation a per-block limit or rule needs. Asserted against literal prefixes
	// rather than against a mask computed the same way the implementation computes it -- a
	// symmetric off-by-one in a shift would cancel out and pass.
	ASSERT_EQUALS(wxString("192.0.2.0"),
		CNetworkAddress::FromString("192.0.2.130").TruncatedToPrefix(24).ToWxString());
	ASSERT_EQUALS(wxString("192.0.0.0"),
		CNetworkAddress::FromString("192.0.2.130").TruncatedToPrefix(16).ToWxString());
	ASSERT_EQUALS(wxString("0.0.0.0"),
		CNetworkAddress::FromString("192.0.2.130").TruncatedToPrefix(0).ToWxString());
	// A prefix at or beyond the family width is the address itself, not an
	// undefined shift.
	ASSERT_EQUALS(wxString("192.0.2.130"),
		CNetworkAddress::FromString("192.0.2.130").TruncatedToPrefix(32).ToWxString());
	ASSERT_EQUALS(wxString("192.0.2.130"),
		CNetworkAddress::FromString("192.0.2.130").TruncatedToPrefix(128).ToWxString());

	// IPv6, including a prefix that ends mid-byte -- /60 keeps the high nibble
	// of the eighth byte and clears the low one.
	ASSERT_EQUALS(wxString("2001:db8:1::"),
		CNetworkAddress::FromString("2001:db8:1:2:3:4:5:6").TruncatedToPrefix(48).ToWxString());
	ASSERT_EQUALS(wxString("2001:db8:1:f0::"),
		CNetworkAddress::FromString("2001:db8:1:f2:3:4:5:6").TruncatedToPrefix(60).ToWxString());
	ASSERT_EQUALS(wxString("2001:db8:1:2::"),
		CNetworkAddress::FromString("2001:db8:1:2:3:4:5:6").TruncatedToPrefix(64).ToWxString());
	ASSERT_EQUALS(wxString("::"),
		CNetworkAddress::FromString("2001:db8:1:2:3:4:5:6").TruncatedToPrefix(0).ToWxString());
	ASSERT_TRUE(CNetworkAddress::FromString("2001:db8:1:2:3:4:5:6").TruncatedToPrefix(128) ==
		    CNetworkAddress::FromString("2001:db8:1:2:3:4:5:6"));

	// A prefix of an absent address is still absent: no prefix is invented for
	// a peer that has no address.
	ASSERT_TRUE(CNetworkAddress::Absent().TruncatedToPrefix(64).IsAbsent());

	// The truncation stays inside its family. A /24 of an IPv4 address is an
	// IPv4 address, and no prefix width turns one family into the other.
	ASSERT_TRUE(CNetworkAddress::FromString("192.0.2.130").TruncatedToPrefix(24).IsIPv4());
	ASSERT_TRUE(CNetworkAddress::FromString("2001:db8::1").TruncatedToPrefix(64).IsIPv6());
}

// The IPv4 half of the same contract, and the one with the sharper failure. A wrong prefix here
// does not advertise an unreachable address, it feeds EncryptedDatagramSocket's key derivation an
// address the peer never sees -- so every frame decrypts to noise at the far end with nothing
// logged on either side. Three of the rejections are sub-byte masks, and a mask is exactly the kind
// of thing that is wrong in one direction only, so each is pinned at both of its edges rather than
// at one address inside it.
TEST(NetworkAddress, GloballyRoutableIPv4RejectsEveryUnroutableRange)
{
	// Routable: ordinary public unicast, and the last address before the
	// multicast floor.
	ASSERT_TRUE(CNetworkAddress::FromString("1.1.1.1").IsGloballyRoutableIPv4());
	ASSERT_TRUE(CNetworkAddress::FromString("223.255.255.255").IsGloballyRoutableIPv4());

	// Not an IPv4 address at all.
	ASSERT_FALSE(CNetworkAddress::Absent().IsGloballyRoutableIPv4());
	ASSERT_FALSE(CNetworkAddress::FromString("2001:db8::1").IsGloballyRoutableIPv4());
	ASSERT_FALSE(CNetworkAddress::FromString("0.0.0.0").IsGloballyRoutableIPv4());

	// A v4-mapped address is judged as the IPv4 address it carries, in both directions --
	// Unmapped() runs first, so the answer must not depend on which form the caller happened to
	// hold.
	ASSERT_TRUE(CNetworkAddress::FromString("::ffff:1.1.1.1").IsGloballyRoutableIPv4());
	ASSERT_FALSE(CNetworkAddress::FromString("::ffff:10.0.0.1").IsGloballyRoutableIPv4());

	// Whole first octets: "this network", RFC 1918 /8, and loopback.
	ASSERT_FALSE(CNetworkAddress::FromString("0.1.2.3").IsGloballyRoutableIPv4());
	ASSERT_FALSE(CNetworkAddress::FromString("10.0.0.1").IsGloballyRoutableIPv4());
	ASSERT_FALSE(CNetworkAddress::FromString("10.255.255.255").IsGloballyRoutableIPv4());
	// The whole 127/8 block, not just 127.0.0.1: a Debian host names itself
	// 127.0.1.1, and that is the value GetPublicIP() falls through to.
	ASSERT_FALSE(CNetworkAddress::FromString("127.0.0.1").IsGloballyRoutableIPv4());
	ASSERT_FALSE(CNetworkAddress::FromString("127.0.1.1").IsGloballyRoutableIPv4());
	ASSERT_FALSE(CNetworkAddress::FromString("127.255.255.255").IsGloballyRoutableIPv4());
	// 126 and 128 bracket it, so the test would fail an off-by-one on the
	// octet as well as a wrong block.
	ASSERT_TRUE(CNetworkAddress::FromString("126.0.0.1").IsGloballyRoutableIPv4());
	ASSERT_TRUE(CNetworkAddress::FromString("128.0.0.1").IsGloballyRoutableIPv4());

	// 100.64.0.0/10, carrier-grade NAT. Mask (b & 0xC0) == 0x40, so the block
	// runs 100.64 through 100.127 and its neighbours must stay routable.
	ASSERT_FALSE(CNetworkAddress::FromString("100.64.0.1").IsGloballyRoutableIPv4());
	ASSERT_FALSE(CNetworkAddress::FromString("100.127.255.255").IsGloballyRoutableIPv4());
	ASSERT_TRUE(CNetworkAddress::FromString("100.63.255.255").IsGloballyRoutableIPv4());
	ASSERT_TRUE(CNetworkAddress::FromString("100.128.0.0").IsGloballyRoutableIPv4());

	// 169.254.0.0/16, link-local.
	ASSERT_FALSE(CNetworkAddress::FromString("169.254.1.1").IsGloballyRoutableIPv4());
	ASSERT_TRUE(CNetworkAddress::FromString("169.253.1.1").IsGloballyRoutableIPv4());
	ASSERT_TRUE(CNetworkAddress::FromString("169.255.1.1").IsGloballyRoutableIPv4());

	// 172.16.0.0/12, private. Mask (b & 0xF0) == 16, so 172.16 through
	// 172.31, and 172.15 and 172.32 are public.
	ASSERT_FALSE(CNetworkAddress::FromString("172.16.0.1").IsGloballyRoutableIPv4());
	ASSERT_FALSE(CNetworkAddress::FromString("172.31.255.255").IsGloballyRoutableIPv4());
	ASSERT_TRUE(CNetworkAddress::FromString("172.15.255.255").IsGloballyRoutableIPv4());
	ASSERT_TRUE(CNetworkAddress::FromString("172.32.0.0").IsGloballyRoutableIPv4());

	// The /24 exclusions under 192.0/16, and private 192.168/16.
	ASSERT_FALSE(CNetworkAddress::FromString("192.0.0.1").IsGloballyRoutableIPv4());
	ASSERT_FALSE(CNetworkAddress::FromString("192.0.2.1").IsGloballyRoutableIPv4());
	ASSERT_FALSE(CNetworkAddress::FromString("192.88.99.0").IsGloballyRoutableIPv4());
	ASSERT_FALSE(CNetworkAddress::FromString("192.88.99.255").IsGloballyRoutableIPv4());
	ASSERT_FALSE(CNetworkAddress::FromString("192.168.1.1").IsGloballyRoutableIPv4());
	ASSERT_TRUE(CNetworkAddress::FromString("192.0.1.1").IsGloballyRoutableIPv4());
	ASSERT_TRUE(CNetworkAddress::FromString("192.88.98.255").IsGloballyRoutableIPv4());
	ASSERT_TRUE(CNetworkAddress::FromString("192.88.100.0").IsGloballyRoutableIPv4());
	ASSERT_TRUE(CNetworkAddress::FromString("192.167.1.1").IsGloballyRoutableIPv4());
	ASSERT_TRUE(CNetworkAddress::FromString("192.169.1.1").IsGloballyRoutableIPv4());

	// 198.18.0.0/15, benchmarking. Mask (b & 0xFE) == 18, so 198.18 and
	// 198.19 only. 198.51.100.0/24 is TEST-NET-2 under the same first octet.
	ASSERT_FALSE(CNetworkAddress::FromString("198.18.0.1").IsGloballyRoutableIPv4());
	ASSERT_FALSE(CNetworkAddress::FromString("198.19.255.255").IsGloballyRoutableIPv4());
	ASSERT_TRUE(CNetworkAddress::FromString("198.17.255.255").IsGloballyRoutableIPv4());
	ASSERT_TRUE(CNetworkAddress::FromString("198.20.0.0").IsGloballyRoutableIPv4());
	ASSERT_FALSE(CNetworkAddress::FromString("198.51.100.1").IsGloballyRoutableIPv4());
	ASSERT_TRUE(CNetworkAddress::FromString("198.51.99.1").IsGloballyRoutableIPv4());
	ASSERT_TRUE(CNetworkAddress::FromString("198.51.101.1").IsGloballyRoutableIPv4());

	// 203.0.113.0/24, TEST-NET-3.
	ASSERT_FALSE(CNetworkAddress::FromString("203.0.113.1").IsGloballyRoutableIPv4());
	ASSERT_TRUE(CNetworkAddress::FromString("203.0.112.1").IsGloballyRoutableIPv4());
	ASSERT_TRUE(CNetworkAddress::FromString("203.0.114.1").IsGloballyRoutableIPv4());

	// Everything from 224 up: multicast and reserved. Neither is a unicast
	// source address, and the boundary is the one the predicate states.
	ASSERT_FALSE(CNetworkAddress::FromString("224.0.0.1").IsGloballyRoutableIPv4());
	ASSERT_FALSE(CNetworkAddress::FromString("239.255.255.255").IsGloballyRoutableIPv4());
	ASSERT_FALSE(CNetworkAddress::FromString("240.0.0.1").IsGloballyRoutableIPv4());
	ASSERT_FALSE(CNetworkAddress::FromString("255.255.255.255").IsGloballyRoutableIPv4());
}

// CNetworkAddress no longer stores a boost::asio::ip::address, so the three predicates that used to
// be asio's -- loopback, link-local, unique-local -- are now prefix tests in NetworkAddress.h. This
// pins each range it must reject, because getting one prefix wrong here does not fail a build: it
// advertises an address no peer can reach, and the only symptom is a wasted connect attempt on the
// far side.
TEST(NetworkAddress, GloballyRoutableIPv6RejectsEveryUnreachableRange)
{
	ASSERT_FALSE(CNetworkAddress::FromString("2001:db8::1").IsGloballyRoutableIPv6());
	for (const auto &prefix : NetworkAddressPolicy::kIPv6ExcludedPrefixes) {
		ASSERT_TRUE(prefix.name[0] != '\0');
		ASSERT_TRUE(prefix.bits > 0 && prefix.bits <= 128);
		const auto first = CNetworkAddress::IPv6FromOctets(prefix.bytes);
		auto lastBytes = prefix.bytes;
		for (unsigned bit = prefix.bits; bit < 128; ++bit) {
			lastBytes[bit / 8] |= static_cast<std::uint8_t>(0x80u >> (bit % 8));
		}
		ASSERT_FALSE(first.IsGloballyRoutableIPv6());
		ASSERT_FALSE(CNetworkAddress::IPv6FromOctets(lastBytes).IsGloballyRoutableIPv6());
		ASSERT_TRUE(NetworkAddressPolicy::MatchesPrefix(first.GetOctets(), prefix));
		ASSERT_TRUE(NetworkAddressPolicy::MatchesPrefix(lastBytes, prefix));
		// Flip each prefix bit: none may be ignored, including partial bytes.
		for (unsigned bit = 0; bit < prefix.bits; ++bit) {
			auto outside = prefix.bytes;
			outside[bit / 8] ^= static_cast<std::uint8_t>(0x80u >> (bit % 8));
			ASSERT_FALSE(NetworkAddressPolicy::MatchesPrefix(outside, prefix));
		}
	}
	// Literal bounds are independent of the production exclusion table: removing
	// an entry or changing its prefix length must not change these expectations.
	const struct
	{
		const char *before;
		const char *first;
		const char *last;
		const char *after;
	} excludedRanges[] = {
		// 64:ff9b::/96
		{ "64:ff9a:ffff:ffff:ffff:ffff:ffff:ffff",
			"64:ff9b::",
			"64:ff9b::ffff:ffff",
			"64:ff9b::1:0:0" },
		// 64:ff9b:1::/48
		{ "64:ff9b:0:ffff:ffff:ffff:ffff:ffff",
			"64:ff9b:1::",
			"64:ff9b:1:ffff:ffff:ffff:ffff:ffff",
			"64:ff9b:2::" },
		// 100::/64
		{ "ff:ffff:ffff:ffff:ffff:ffff:ffff:ffff",
			"100::",
			"100::ffff:ffff:ffff:ffff",
			"100:0:0:1::" },
		// 2001:20::/28
		{ "2001:1f:ffff:ffff:ffff:ffff:ffff:ffff",
			"2001:20::",
			"2001:2f:ffff:ffff:ffff:ffff:ffff:ffff",
			"2001:30::" },
		// 2001:db8::/32
		{ "2001:db7:ffff:ffff:ffff:ffff:ffff:ffff",
			"2001:db8::",
			"2001:db8:ffff:ffff:ffff:ffff:ffff:ffff",
			"2001:db9::" },
		// 5f00::/16
		{ "5eff:ffff:ffff:ffff:ffff:ffff:ffff:ffff",
			"5f00::",
			"5f00:ffff:ffff:ffff:ffff:ffff:ffff:ffff",
			"5f01::" },
	};
	for (const auto &range : excludedRanges) {
		ASSERT_TRUE(CNetworkAddress::FromString(range.before).IsGloballyRoutableIPv6());
		ASSERT_FALSE(CNetworkAddress::FromString(range.first).IsGloballyRoutableIPv6());
		ASSERT_FALSE(CNetworkAddress::FromString(range.last).IsGloballyRoutableIPv6());
		ASSERT_TRUE(CNetworkAddress::FromString(range.after).IsGloballyRoutableIPv6());
	}
	ASSERT_TRUE(CNetworkAddress::FromString("2606:4700::1111").IsGloballyRoutableIPv6());

	// Not an IPv6 address at all.
	ASSERT_FALSE(CNetworkAddress::Absent().IsGloballyRoutableIPv6());
	ASSERT_FALSE(CNetworkAddress::FromString("192.0.2.1").IsGloballyRoutableIPv6());
	// A mapped address is reachable over IPv4, so it is not an IPv6 address to
	// advertise as one.
	ASSERT_FALSE(CNetworkAddress::FromString("::ffff:192.0.2.1").IsGloballyRoutableIPv6());

	// The unspecified address and loopback.
	ASSERT_FALSE(CNetworkAddress::AnyIPv6().IsGloballyRoutableIPv6());
	ASSERT_FALSE(CNetworkAddress::FromString("::").IsGloballyRoutableIPv6());
	ASSERT_FALSE(CNetworkAddress::FromString("::1").IsGloballyRoutableIPv6());

	// fe80::/10, link-local -- both ends of the range, since the prefix is ten
	// bits and a byte-wide test would let the upper half through.
	ASSERT_FALSE(CNetworkAddress::FromString("fe80::1").IsGloballyRoutableIPv6());
	ASSERT_FALSE(CNetworkAddress::FromString("febf:ffff::1").IsGloballyRoutableIPv6());

	// fec0::/10, the deprecated site-local range.
	ASSERT_FALSE(CNetworkAddress::FromString("fec0::1").IsGloballyRoutableIPv6());
	ASSERT_FALSE(CNetworkAddress::FromString("feff:ffff::1").IsGloballyRoutableIPv6());

	// Teredo 2001::/32 and 6to4 2002::/16.
	ASSERT_FALSE(CNetworkAddress::FromString("2001::").IsGloballyRoutableIPv6());
	ASSERT_FALSE(CNetworkAddress::FromString("2001::ffff:ffff:ffff:ffff").IsGloballyRoutableIPv6());
	ASSERT_TRUE(CNetworkAddress::FromString("2000:ffff:ffff:ffff:ffff:ffff:ffff:ffff")
			    .IsGloballyRoutableIPv6());
	ASSERT_FALSE(CNetworkAddress::FromString("2002::").IsGloballyRoutableIPv6());
	ASSERT_FALSE(CNetworkAddress::FromString("2002:ffff:ffff:ffff:ffff:ffff:ffff:ffff")
			     .IsGloballyRoutableIPv6());
	ASSERT_TRUE(CNetworkAddress::FromString("2001:ffff:ffff:ffff:ffff:ffff:ffff:ffff")
			    .IsGloballyRoutableIPv6());
	ASSERT_TRUE(CNetworkAddress::FromString("2003::").IsGloballyRoutableIPv6());

	// fc00::/7, unique-local. Both halves: fc00::/8 and fd00::/8.
	ASSERT_FALSE(CNetworkAddress::FromString("fc00::1").IsGloballyRoutableIPv6());
	ASSERT_FALSE(CNetworkAddress::FromString("fd12:3456::1").IsGloballyRoutableIPv6());
	ASSERT_FALSE(CNetworkAddress::FromString("fdff:ffff::1").IsGloballyRoutableIPv6());

	// ff00::/8, multicast.
	ASSERT_FALSE(CNetworkAddress::FromString("ff02::1").IsGloballyRoutableIPv6());

	// And the addresses immediately outside those prefixes stay routable, so
	// the tests above are pinning a prefix and not a whole leading byte.
	ASSERT_TRUE(CNetworkAddress::FromString("fbff:ffff::1").IsGloballyRoutableIPv6());
	ASSERT_TRUE(CNetworkAddress::FromString("fe00::1").IsGloballyRoutableIPv6());
	ASSERT_TRUE(CNetworkAddress::FromString("fe7f:ffff::1").IsGloballyRoutableIPv6());
}

// The octets are the storage now, so what GetOctets() hands out is what every bit-arithmetic caller
// -- IPFilterMatch.h above all -- works on. Two things are worth pinning: the order is wire order
// for both families, and an IPv4 address leaves the tail zero rather than filling in the mapped
// prefix. Callers relying on the latter would silently match the wrong rule.
TEST(NetworkAddress, OctetsAreWireOrderAndLeaveTheIPv4TailZero)
{
	// 192.0.2.1 reached through either 32-bit convention gives the same octets,
	// most significant first -- neither convention is the storage order.
	const CNetworkAddress v4 = CNetworkAddress::FromIPv4NetworkOrder(TEST_IP_ED2K_ORDER);
	const CNetworkAddress::Octets v4Octets = v4.GetOctets();
	ASSERT_EQUALS(192, static_cast<int>(v4Octets[0]));
	ASSERT_EQUALS(0, static_cast<int>(v4Octets[1]));
	ASSERT_EQUALS(2, static_cast<int>(v4Octets[2]));
	ASSERT_EQUALS(1, static_cast<int>(v4Octets[3]));
	for (int i = 4; i < 16; ++i) {
		ASSERT_EQUALS(0, static_cast<int>(v4Octets[i]));
	}
	ASSERT_EQUALS(0ul, v4.GetScopeId());

	// The mapped form of the same address is a different set of octets, which
	// is the storage-level statement of MappedAndNativeAreDistinct above.
	const CNetworkAddress::Octets mappedOctets =
		CNetworkAddress::FromString("::ffff:192.0.2.1").GetOctets();
	ASSERT_EQUALS(0xff, static_cast<int>(mappedOctets[10]));
	ASSERT_EQUALS(0xff, static_cast<int>(mappedOctets[11]));
	ASSERT_EQUALS(192, static_cast<int>(mappedOctets[12]));
	ASSERT_EQUALS(1, static_cast<int>(mappedOctets[15]));
	ASSERT_TRUE(v4Octets != mappedOctets);

	// IPv6FromOctets() applies no absence rule, so the all-zero octets are the unspecified
	// address and not absence -- unlike FromIPv6Bytes(), the wire-tag edge where all-zero does
	// mean "this peer has no IPv6".
	ASSERT_TRUE(CNetworkAddress::AnyIPv6().IsPresent());
	ASSERT_TRUE(CNetworkAddress::AnyIPv6().IsIPv6());
	ASSERT_TRUE(CNetworkAddress::AnyIPv6().IsUnspecified());
	ASSERT_EQUALS(wxString("::"), CNetworkAddress::AnyIPv6().ToWxString());
	const std::uint8_t allZero[16] = { 0 };
	ASSERT_TRUE(CNetworkAddress::FromIPv6Bytes(allZero).IsAbsent());
	ASSERT_TRUE(CNetworkAddress::AnyIPv6() != CNetworkAddress::Absent());
}

// ToIPv6Bytes() is the wire-side writer: the CT_MOD_IP_V6 handshake tag, Kad's "ip6" tag and the
// NAT endpoint hint each hand it a bare sixteen-byte buffer and then read all sixteen back. Nothing
// pinned that it fills all sixteen, so the postcondition lived only in the doc comment -- and a
// short write there is not cosmetic: the bytes it left alone become part of an address a peer is
// told to punch at.
TEST(NetworkAddress, ToIPv6BytesFillsAllSixteenOrWritesNothing)
{
	// Sentinel fill rather than zero fill: a byte still holding 0xCD afterwards is a byte the
	// writer skipped, which a zero-filled buffer would hide behind a plausible-looking 0.
	std::uint8_t bytes[16];
	std::fill(std::begin(bytes), std::end(bytes), 0xCD);

	const CNetworkAddress v6 = CNetworkAddress::FromString("2001:db8::1");
	ASSERT_TRUE(v6.ToIPv6Bytes(bytes));
	const CNetworkAddress::Octets &expected = v6.GetOctets();
	for (std::size_t i = 0; i < expected.size(); ++i) {
		ASSERT_EQUALS(static_cast<int>(expected[i]), static_cast<int>(bytes[i]));
	}
	// Stated separately for the trailing octet, because the tail is what a copy that stops
	// early loses first. It is 1 rather than 0 here, so "skipped" and "correctly written"
	// cannot coincide.
	ASSERT_EQUALS(1, static_cast<int>(bytes[15]));

	// A mapped IPv4 address is an IPv6 address for this writer's purposes, and it
	// goes out as the mapped form -- all sixteen octets again, not four.
	std::fill(std::begin(bytes), std::end(bytes), 0xCD);
	ASSERT_TRUE(CNetworkAddress::FromString("::ffff:192.0.2.1").ToIPv6Bytes(bytes));
	ASSERT_EQUALS(0xff, static_cast<int>(bytes[10]));
	ASSERT_EQUALS(0xff, static_cast<int>(bytes[11]));
	ASSERT_EQUALS(192, static_cast<int>(bytes[12]));
	ASSERT_EQUALS(1, static_cast<int>(bytes[15]));

	// The failure paths write nothing at all, so a caller that ignores the result
	// cannot mistake a stale buffer for an address it never received.
	std::fill(std::begin(bytes), std::end(bytes), 0xCD);
	ASSERT_FALSE(CNetworkAddress::FromIPv4HostOrder(TEST_IP_HOST_ORDER).ToIPv6Bytes(bytes));
	ASSERT_FALSE(CNetworkAddress::Absent().ToIPv6Bytes(bytes));
	ASSERT_FALSE(v6.ToIPv6Bytes(nullptr));
	for (std::size_t i = 0; i < 16; ++i) {
		ASSERT_EQUALS(0xCD, static_cast<int>(bytes[i]));
	}
}

// The socket backend is the one caller that still needs a real asio address, so
// NetworkAddressAsio.h is the single bridge. It has to be exactly lossless in both directions: a
// swapped IPv4 conversion here would connect to the wrong host, and a dropped scope id would fold
// two distinct link-local destinations into one.
TEST(NetworkAddress, AsioBridgeRoundTripsWithoutLosingAnything)
{
	const CNetworkAddress cases[] = {
		CNetworkAddress::FromIPv4NetworkOrder(TEST_IP_ED2K_ORDER),
		CNetworkAddress::FromString("0.0.0.0"),
		CNetworkAddress::FromString("255.255.255.254"),
		CNetworkAddress::FromString("2001:db8::1"),
		CNetworkAddress::FromString("::ffff:192.0.2.1"),
		CNetworkAddress::AnyIPv6(),
		CNetworkAddress::FromString("::1"),
	};
	for (const CNetworkAddress &address : cases) {
		const CNetworkAddress roundTripped =
			NetworkAddressAsio::FromAsioAddress(NetworkAddressAsio::ToAsioAddress(address));
		ASSERT_TRUE(roundTripped == address);
		// Not just equal -- the same text, so a family or octet swap that
		// happened to compare equal would still be caught.
		ASSERT_EQUALS(address.ToWxString(), roundTripped.ToWxString());
	}

	// The 32-bit conventions survive the crossing: asio's to_uint() is host
	// order, which is the convention FromIPv4HostOrder() names.
	ASSERT_EQUALS(wxString("192.0.2.1"),
		NetworkAddressAsio::FromAsioAddress(boost::asio::ip::make_address("192.0.2.1")).ToWxString());
	std::uint32_t hostOrder = 0;
	ASSERT_TRUE(NetworkAddressAsio::FromAsioAddress(boost::asio::ip::make_address("192.0.2.1"))
			    .ToIPv4HostOrder(hostOrder));
	ASSERT_EQUALS(TEST_IP_HOST_ORDER, hostOrder);

	// A scope id is part of the address's identity, so it crosses too. Without it fe80::1%7 and
	// fe80::1%9 would be one key in every container that uses this type.
	const CNetworkAddress scoped =
		NetworkAddressAsio::FromAsioAddress(boost::asio::ip::make_address("fe80::1%7"));
	ASSERT_EQUALS(7ul, scoped.GetScopeId());
	ASSERT_TRUE(scoped == NetworkAddressAsio::FromAsioAddress(NetworkAddressAsio::ToAsioAddress(scoped)));
	ASSERT_TRUE(scoped != CNetworkAddress::FromString("fe80::1"));
	ASSERT_TRUE(CNetworkAddress::FromString("fe80::1").GetScopeId() == 0ul);

	// An asio address is always a present address: nothing it can hold means
	// absence, 0.0.0.0 included. That overload belongs to the wire edges.
	ASSERT_TRUE(
		NetworkAddressAsio::FromAsioAddress(boost::asio::ip::make_address("0.0.0.0")).IsPresent());
	ASSERT_TRUE(NetworkAddressAsio::FromAsioAddress(boost::asio::ip::make_address("0.0.0.0"))
			    .IsUnspecified());
}

// File_checked_for_headers
