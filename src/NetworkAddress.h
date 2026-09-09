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

#ifndef NETWORKADDRESS_H
#define NETWORKADDRESS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include <wx/string.h>

namespace NetworkAddressPolicy
{
struct IPv6ExcludedPrefix
{
	std::array<std::uint8_t, 16> bytes;
	unsigned bits;
	const char *name;
};

constexpr IPv6ExcludedPrefix kIPv6ExcludedPrefixes[] = { { {}, 128, "Unspecified" },
	{ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 }, 128, "Loopback" },
	{ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff }, 96, "IPv4-mapped" },
	{ { 0x00, 0x64, 0xff, 0x9b }, 96, "Well-known NAT64" },
	{ { 0x00, 0x64, 0xff, 0x9b, 0x00, 0x01 }, 48, "Local-use NAT64" },
	{ { 0x01, 0x00 }, 64, "Discard-only" },
	{ { 0x20, 0x01 }, 32, "Teredo" },
	{ { 0x20, 0x01, 0x00, 0x20 }, 28, "ORCHIDv2" },
	{ { 0x20, 0x01, 0x0d, 0xb8 }, 32, "Documentation" },
	{ { 0x20, 0x02 }, 16, "6to4" },
	{ { 0x5f, 0x00 }, 16, "Segment routing SIDs" },
	{ { 0xfc }, 7, "Unique-local" },
	{ { 0xfe, 0x80 }, 10, "Link-local" },
	{ { 0xfe, 0xc0 }, 10, "Deprecated site-local" },
	{ { 0xff }, 8, "Multicast" } };

inline bool MatchesPrefix(
	const std::array<std::uint8_t, 16> &address, const IPv6ExcludedPrefix &prefix) noexcept
{
	for (unsigned bit = 0; bit < prefix.bits; ++bit) {
		const unsigned mask = 0x80u >> (bit % 8);
		if ((address[bit / 8] & mask) != (prefix.bytes[bit / 8] & mask)) {
			return false;
		}
	}
	return true;
}
} // namespace NetworkAddressPolicy

/**
 * The internal, family-agnostic address type.
 *
 * aMule stores IP addresses as bare @c uint32 across Kademlia, the client list
 * and the IP filter. That representation cannot hold an IPv6 address, and it
 * carries two further defects that a plain widening would silently inherit:
 *
 *  - **Byte order is not in the type.** A @c uint32 IP in this tree is in one
 *    of two conventions depending on the call site, and the two are bridged by
 *    scattered @c wxUINT32_SWAP_ALWAYS calls. Which convention a value is in
 *    is documented in comments at best.
 *  - **Zero doubles as "no address".** So a client whose address is genuinely
 *    unknown is indistinguishable from one at @c 0.0.0.0.
 *
 * This type fixes both by construction. There is no implicit conversion to or
 * from @c uint32: every narrowing and widening goes through a named function
 * whose name states the byte order, and absence is a state of the object
 * rather than a magic value.
 *
 * ## Why this header pulls in no socket library
 *
 * The address used to be stored as an @c asio::ip::address, borrowing
 * asio's v4/v6 variant rather than restating it. That was cheap to write and
 * expensive to compile: this header is reached by eighteen public headers --
 * updownclient.h, ClientList.h, IPFilter.h, DownloadQueue.h among them -- so
 * asio ended up in the include closure of 155 of the 254 translation units in
 * src/, and it took two whole platforms down with it:
 *
 *  - Asio's @c ip/address.hpp pulls @c asio/detail/winsock_init.hpp, whose
 *    static initialiser references @c WSAStartup / @c WSACleanup. Every one of
 *    those 155 TUs therefore needed @c ws2_32 at link time, and the targets
 *    that do not link it failed on mingw-w64 -- for an address type that never
 *    opens a socket.
 *  - It is a large closure (about 1200 headers) compiled 155 times over for
 *    what is, in the end, sixteen bytes and a family tag.
 *
 * So the storage is now those sixteen bytes and that family tag, spelled out
 * here. Only the two operations that genuinely need a library -- parsing a
 * textual address and formatting one -- still use asio, and they live in
 * NetworkAddress.cpp where exactly one TU pays for them. Nothing about IPv6
 * text handling is hand-rolled: RFC 4291 zero compression, the IPv4-mapped
 * dotted-quad tail and scope-id suffixes are all still asio's job.
 *
 * A caller that needs the asio value itself -- the socket backend, and only
 * it -- gets it from NetworkAddressAsio.h, which is the one bridge and is
 * included by the TUs that truly open sockets.
 *
 * ## The two 32-bit conventions
 *
 * For the address @c 192.0.2.1:
 *
 * | Convention | Value | Used by |
 * | --- | --- | --- |
 * | host order (numeric) | @c 0xC0000201 | Kademlia (`KadIPToString`) |
 * | network order | @c 0x010200C0 | ed2k core (`Uint32toStringIP`) |
 *
 * "Network order" is the name used in this type's API for what the rest of
 * aMule's comments call *anti-host order*: the four octets packed with the
 * first octet in the numerically least significant byte. On a little-endian
 * host that is the value obtained by loading four network-order bytes straight
 * into a @c uint32, which is how it arises in practice -- an ed2k packet field
 * read by CFileDataIO. The two conventions are exact byte reversals of each
 * other on every platform, which is why @c wxUINT32_SWAP_ALWAYS converts
 * between them unconditionally.
 *
 * Note that neither convention is the storage order. Internally the address is
 * always its octets in wire order, most significant first, so that the v4 and
 * v6 cases need no separate code path and the comparison rule below is one
 * lexicographic compare rather than a family switch.
 *
 * ## Comparison rule
 *
 * One rule, applied everywhere:
 *
 *  1. An absent address sorts before every present one, and equals only
 *     another absent address.
 *  2. IPv4 sorts before IPv6.
 *  3. Within a family, addresses sort by their octets, most significant
 *     first.
 *
 * IPv4-mapped forms are **not** normalised: @c ::ffff:192.0.2.1 is an IPv6
 * address, so it is neither equal to nor adjacent to the IPv4 address
 * @c 192.0.2.1. This keeps the ordering total with no two distinct addresses
 * comparing equal, which is what makes the type safe as a container key.
 * Callers that want the two treated alike must say so by calling Unmapped()
 * first.
 */
class CNetworkAddress
{
public:
	/** How many octets an address of either family occupies. */
	static constexpr std::size_t OCTET_COUNT = 16;

	/** The sixteen octets an IPv6 address occupies, in wire order. */
	using Octets = std::array<std::uint8_t, OCTET_COUNT>;

	/**
	 * Which family a present address belongs to, or that there is none.
	 *
	 * Absence is a case of this enum rather than a wrapping std::optional so
	 * that the object stays trivially copyable and one word smaller, and so
	 * that the three-way switch the comparison rule needs is a switch on one
	 * field instead of a nested optional test.
	 */
	enum class Family : std::uint8_t
	{
		None,
		IPv4,
		IPv6
	};

	/** Constructs an absent address -- @b not @c 0.0.0.0. */
	CNetworkAddress() = default;

	/** An explicitly absent address, for call sites where the name reads better. */
	static CNetworkAddress Absent() { return CNetworkAddress(); }

	/**
	 * Builds an IPv4 address from a 32-bit value in host (numeric) order,
	 * i.e. @c 192.0.2.1 is @c 0xC0000201. This is the Kademlia convention.
	 */
	static CNetworkAddress FromIPv4HostOrder(std::uint32_t ip)
	{
		CNetworkAddress result;
		result.m_family = Family::IPv4;
		result.m_octets[0] = static_cast<std::uint8_t>((ip >> 24) & 0xFFu);
		result.m_octets[1] = static_cast<std::uint8_t>((ip >> 16) & 0xFFu);
		result.m_octets[2] = static_cast<std::uint8_t>((ip >> 8) & 0xFFu);
		result.m_octets[3] = static_cast<std::uint8_t>(ip & 0xFFu);
		return result;
	}

	/**
	 * Builds an IPv4 address from a 32-bit value in network order, i.e.
	 * @c 192.0.2.1 is @c 0x010200C0. This is the ed2k convention, called
	 * "anti-host order" elsewhere in the tree.
	 */
	static CNetworkAddress FromIPv4NetworkOrder(std::uint32_t ip)
	{
		return FromIPv4HostOrder(SwapOctets(ip));
	}

	/**
	 * The conversion boundary for a 32-bit ed2k-order address field in which
	 * zero is overloaded to mean "no address".
	 *
	 * This is the lossy direction and the only place the overload is allowed to
	 * live. A caller holding such a field cannot tell @c 0.0.0.0 from "unknown",
	 * so this resolves the ambiguity once, at the edge, in favour of absence --
	 * which is what the @c if (ip) tests it replaces already did. Use it only
	 * where the old code actually tested the value against zero; where zero was
	 * simply a value, FromIPv4NetworkOrder() keeps behaviour identical.
	 */
	static CNetworkAddress FromIPv4NetworkOrderOrAbsent(std::uint32_t ip)
	{
		return ip == 0 ? Absent() : FromIPv4NetworkOrder(ip);
	}

	/** As FromIPv4NetworkOrderOrAbsent(), for the Kad host-order convention. */
	static CNetworkAddress FromIPv4HostOrderOrAbsent(std::uint32_t ip)
	{
		return ip == 0 ? Absent() : FromIPv4HostOrder(ip);
	}

	/**
	 * Builds an IPv6 address from its octets, with no interpretation of their
	 * value at all -- the all-zero octets give the unspecified address @c :: ,
	 * which is a real address here and not absence.
	 *
	 * This is the raw widening, for callers that already know they hold an
	 * address: the asio bridge reconstructing one, and AnyIPv6() below. A
	 * caller decoding a wire tag wants FromIPv6Bytes() instead, which applies
	 * that edge's absence rule.
	 */
	static CNetworkAddress IPv6FromOctets(const Octets &octets, unsigned long scopeId = 0)
	{
		CNetworkAddress result;
		result.m_family = Family::IPv6;
		result.m_octets = octets;
		result.m_scopeId = scopeId;
		return result;
	}

	/**
	 * The IPv6 wildcard, @c :: -- present, IPv6 and unspecified.
	 *
	 * Named rather than left to IPv6FromOctets({}) because the call sites that
	 * want it are binding a listening socket to "every local address", and at
	 * those sites @c :: is a decision about reachability, not sixteen zero
	 * bytes. It also keeps them from reaching for the asio-typed wildcard in
	 * AddressFamilyPolicyAsio.h, which would put asio back in their closure.
	 */
	static CNetworkAddress AnyIPv6() { return IPv6FromOctets(Octets{}); }

	/**
	 * Builds an IPv6 address from sixteen big-endian bytes -- the form the
	 * @c CT_MOD_IP_V6 hello tag and the Kad @c "ip6" tag carry.
	 *
	 * @return The address, or an @b absent address when @a bytes is NULL or
	 *         the sixteen bytes are all zero. The all-zero value is @c :: ,
	 *         which is what a peer that has no IPv6 address sends when it
	 *         emits the tag anyway; treating it as an address would have aMule
	 *         dialling the unspecified address.
	 */
	static CNetworkAddress FromIPv6Bytes(const std::uint8_t *bytes)
	{
		if (bytes == nullptr) {
			return Absent();
		}
		Octets raw;
		bool allZero = true;
		for (std::size_t i = 0; i < raw.size(); ++i) {
			raw[i] = bytes[i];
			if (bytes[i] != 0) {
				allZero = false;
			}
		}
		if (allZero) {
			return Absent();
		}
		return IPv6FromOctets(raw);
	}

	/**
	 * Parses a textual address, IPv4 or IPv6.
	 *
	 * Defined in NetworkAddress.cpp: this is one of the two operations that
	 * still go through asio, so that the accepted syntax stays exactly a
	 * library's idea of an address literal rather than this file's.
	 *
	 * @return The address, or an @b absent address if @a text is not a valid
	 *         literal. Note that this never yields @c 0.0.0.0 on failure, unlike
	 *         StringIPtoUint32().
	 */
	static CNetworkAddress FromString(const std::string &text);

	bool IsPresent() const noexcept { return m_family != Family::None; }
	bool IsAbsent() const noexcept { return m_family == Family::None; }

	/** True for a present address whose every octet is zero (@c 0.0.0.0 or @c ::). */
	bool IsUnspecified() const noexcept
	{
		if (IsAbsent()) {
			return false;
		}
		// The unused tail of an IPv4 address is always zero (see m_octets), so
		// one loop over all sixteen answers for both families.
		for (const std::uint8_t octet : m_octets) {
			if (octet != 0) {
				return false;
			}
		}
		return true;
	}

	bool IsIPv4() const noexcept { return m_family == Family::IPv4; }
	bool IsIPv6() const noexcept { return m_family == Family::IPv6; }

	/** True only for an IPv6 address in the @c ::ffff:a.b.c.d form. */
	bool IsIPv4Mapped() const noexcept
	{
		if (!IsIPv6()) {
			return false;
		}
		// The prefix is RFC 4291 section 2.5.5.2: eighty zero bits, then
		// 0xffff. Tested against the octets rather than via asio's
		// address_v6::is_v4_mapped(), which asio deprecated after 1.66 and has
		// since removed -- and which this header no longer has anyway.
		for (int i = 0; i < 10; ++i) {
			if (m_octets[i] != 0) {
				return false;
			}
		}
		return m_octets[10] == 0xff && m_octets[11] == 0xff;
	}

	/**
	 * The address as its sixteen octets in wire order.
	 *
	 * For an IPv6 address these are the address. For IPv4 the four octets sit
	 * in the first four positions and the rest are zero, which is @b not the
	 * IPv4-mapped form -- so do not hand these to something expecting sixteen
	 * IPv6 octets without checking the family first. For an absent address they
	 * are all zero, which is exactly the conflation this type exists to
	 * prevent, so check IsPresent() too.
	 *
	 * This exists so that callers doing bit arithmetic on an address -- prefix
	 * matching in IPFilterMatch.h, chiefly -- can do it without a library and
	 * without this header growing one. Where the caller wants the guardrails,
	 * ToIPv6Bytes() below states the family requirement in its return value.
	 */
	const Octets &GetOctets() const noexcept { return m_octets; }

	/**
	 * The interface scope of an IPv6 address, or zero when it has none.
	 *
	 * Carried because it is part of the address's identity: @c fe80::1%eth0 and
	 * @c fe80::1%eth1 are two different destinations, so folding them together
	 * would break the total order this type promises.
	 */
	unsigned long GetScopeId() const noexcept { return m_scopeId; }

	/**
	 * The same address with any IPv4-mapped IPv6 form collapsed to plain IPv4.
	 * Everything else, including absence, is returned unchanged. Call this
	 * deliberately at sites that must treat the mapped and native forms as one
	 * address; the comparison operators never do it for you.
	 */
	CNetworkAddress Unmapped() const
	{
		if (IsIPv4Mapped()) {
			return FromIPv4HostOrder(EmbeddedIPv4HostOrder());
		}
		return *this;
	}

	/**
	 * Narrows to a 32-bit IPv4 value in host (numeric) order.
	 *
	 * @param out Assigned only on success; left untouched on failure, so a
	 *            caller that ignores the result cannot end up with a fabricated
	 *            address.
	 * @return False if the address is absent, or is an IPv6 address that is not
	 *         IPv4-mapped. Such an address has no 32-bit form and this
	 *         deliberately fails rather than truncating or hashing it.
	 */
	bool ToIPv4HostOrder(std::uint32_t &out) const noexcept
	{
		if (IsIPv4()) {
			out = PackOctets(m_octets[0], m_octets[1], m_octets[2], m_octets[3]);
			return true;
		}
		if (IsIPv4Mapped()) {
			out = EmbeddedIPv4HostOrder();
			return true;
		}
		return false;
	}

	/**
	 * Narrows to a 32-bit IPv4 value in network order (ed2k / "anti-host"
	 * order). Same failure contract as ToIPv4HostOrder().
	 */
	bool ToIPv4NetworkOrder(std::uint32_t &out) const noexcept
	{
		std::uint32_t hostOrder = 0;
		if (!ToIPv4HostOrder(hostOrder)) {
			return false;
		}
		out = SwapOctets(hostOrder);
		return true;
	}

	/**
	 * Narrows back to an ed2k-order field that uses zero for "no address",
	 * for handing to an edge this refactor has not reached yet.
	 *
	 * @return Zero for an absent address, and also for an address with no
	 *         32-bit form. Prefer ToIPv4NetworkOrder() wherever the caller can
	 *         act on the difference: this one cannot report it.
	 */
	std::uint32_t ToIPv4NetworkOrderOrZero() const noexcept
	{
		std::uint32_t value = 0;
		ToIPv4NetworkOrder(value);
		return value;
	}

	/**
	 * Writes an IPv6 address out as sixteen big-endian bytes -- the form the
	 * @c CT_MOD_IP_V6 hello tag and the Kad @c "ip6" tag carry.
	 *
	 * @param out Sixteen bytes, written only on success, so a caller that
	 *            ignores the result cannot emit a half-filled address.
	 * @return False for an absent or IPv4 address. An IPv4-mapped one is
	 *         written as the mapped IPv6 address it is: that is what the peer
	 *         asked for when it asked for an IPv6 address.
	 */
	bool ToIPv6Bytes(std::uint8_t *out) const noexcept
	{
		if (out == nullptr || !IsIPv6()) {
			return false;
		}
		// One copy of a constant length, not a loop bounded by m_octets.size().
		// The two are the same sixteen bytes, but only the constant states the
		// postcondition above in a form a reader -- or a static analyser -- can
		// check without first proving what size() returns. The Clang Static
		// Analyzer could not: it explored the zero-iteration path through the
		// old loop and concluded this function returns true having written
		// nothing, which surfaced as garbage-value reports in all four callers
		// that read the buffer back (IPFilterMatch.h, NatRendezvousProtocol.h
		// and, through CMD4Hash, ArchSpecific.h).
		std::memcpy(out, m_octets.data(), OCTET_COUNT);
		return true;
	}

	/**
	 * Whether this is an IPv4 address that can stand for this host on the
	 * public internet: present, IPv4 (or the mapped form of one), and outside
	 * every range the address itself declares unroutable -- the unspecified
	 * address, loopback, the three RFC1918 private blocks, link-local,
	 * carrier-grade NAT, the documentation and benchmark blocks, multicast and
	 * the reserved top of the space.
	 *
	 * The IPv6 sibling below exists so an address is never advertised to a peer
	 * that cannot reach it. This one exists for a second reason, and it is the
	 * sharper of the two: aMule's UDP obfuscation bakes the *sender's* own
	 * public address into the key -- EncryptedDatagramSocket.cpp derives it as
	 * MD5(<receiver user hash 16><sender IP 4><0x5B><random 2>) -- while the
	 * receiver derives its half from the source address of the datagram it
	 * actually got. The two only agree when the sender's idea of its own
	 * address is the one the peer sees. Encrypt with 127.0.1.1 in that field
	 * and every frame decrypts to noise at the far end, with nothing logged on
	 * either side, because a failed obfuscation looks exactly like a peer that
	 * never obfuscated. So "do I have an address a stranger could reply to" has
	 * to be answered before the key is derived, not assumed from non-zero.
	 */
	bool IsGloballyRoutableIPv4() const noexcept
	{
		const CNetworkAddress plain = Unmapped();
		if (!plain.IsIPv4() || plain.IsUnspecified()) {
			return false;
		}
		const std::uint8_t a = plain.m_octets[0];
		const std::uint8_t b = plain.m_octets[1];

		if (a == 0) {
			return false; // 0.0.0.0/8, "this network" (RFC 1122).
		}
		if (a == 10) {
			return false; // 10.0.0.0/8, private (RFC 1918).
		}
		if (a == 100 && (b & 0xC0) == 0x40) {
			return false; // 100.64.0.0/10, carrier-grade NAT (RFC 6598).
		}
		if (a == 127) {
			// 127.0.0.0/8. The whole block, not just 127.0.0.1: a Debian host
			// names itself 127.0.1.1 in /etc/hosts, and that is precisely the
			// value CamuleApp::GetPublicIP() hands back when it falls through
			// to m_localip.
			return false;
		}
		if (a == 169 && b == 254) {
			return false; // 169.254.0.0/16, link-local (RFC 3927).
		}
		if (a == 172 && (b & 0xF0) == 16) {
			return false; // 172.16.0.0/12, private (RFC 1918).
		}
		if (a == 192 && b == 0 && plain.m_octets[2] == 0) {
			return false; // 192.0.0.0/24, IETF protocol assignments.
		}
		if (a == 192 && b == 0 && plain.m_octets[2] == 2) {
			return false; // 192.0.2.0/24, TEST-NET-1 documentation (RFC 5737).
		}
		if (a == 192 && b == 88 && plain.m_octets[2] == 99) {
			return false; // 192.88.99.0/24, IPv6 relay anycast (RFC 3068).
		}
		if (a == 192 && b == 168) {
			return false; // 192.168.0.0/16, private (RFC 1918).
		}
		if (a == 198 && (b & 0xFE) == 18) {
			return false; // 198.18.0.0/15, benchmarking (RFC 2544).
		}
		if (a == 198 && b == 51 && plain.m_octets[2] == 100) {
			return false; // 198.51.100.0/24, TEST-NET-2 (RFC 5737).
		}
		if (a == 203 && b == 0 && plain.m_octets[2] == 113) {
			return false; // 203.0.113.0/24, TEST-NET-3 (RFC 5737).
		}
		// 224.0.0.0/4 multicast and 240.0.0.0/4 reserved, which together are
		// everything from 224 up. Neither is a unicast source address.
		return a < 224;
	}

	/**
	 * Whether this is an IPv6 address worth telling a peer about: present,
	 * IPv6, not mapped, and globally routable as far as the address itself can
	 * say -- so not the unspecified address, not loopback, not link-local and
	 * not a unique-local address.
	 *
	 * Advertising any of those is worse than advertising nothing: the peer
	 * cannot reach them and spends a connect attempt finding out.
	 */
	bool IsGloballyRoutableIPv6() const noexcept
	{
		if (!IsIPv6()) {
			return false;
		}
		for (const auto &prefix : NetworkAddressPolicy::kIPv6ExcludedPrefixes) {
			if (NetworkAddressPolicy::MatchesPrefix(m_octets, prefix)) {
				return false;
			}
		}
		return true;
	}

	/**
	 * The network address of the prefix this address falls in: the same
	 * address with every bit below @a prefixBits cleared.
	 *
	 * Used where a limit or a rule applies to a block rather than to a host --
	 * an IPv6 subscriber is delegated a prefix, not an address, so a per-address
	 * budget under IPv6 counts to one forever (see PeerAddressing.h).
	 *
	 * @param prefixBits Counted from the most significant bit of the address in
	 *                   its own family: 0..32 for IPv4, 0..128 for IPv6. A value
	 *                   at or above the family's width returns the address
	 *                   unchanged.
	 * @return The prefix's network address. An absent address is returned
	 *         unchanged -- there is no prefix to compute and none is invented.
	 */
	CNetworkAddress TruncatedToPrefix(unsigned prefixBits) const
	{
		if (IsAbsent()) {
			return *this;
		}
		const unsigned width = IsIPv4() ? 32u : 128u;
		if (prefixBits >= width) {
			return *this;
		}
		Octets bytes = m_octets;
		for (std::size_t i = 0; i < bytes.size(); ++i) {
			const unsigned bitsBefore = static_cast<unsigned>(i) * 8u;
			if (prefixBits >= bitsBefore + 8u) {
				continue; // Wholly inside the prefix.
			}
			if (prefixBits <= bitsBefore) {
				bytes[i] = 0; // Wholly outside it.
			} else {
				bytes[i] = static_cast<std::uint8_t>(
					bytes[i] & (0xFFu << (bitsBefore + 8u - prefixBits)));
			}
		}
		if (IsIPv4()) {
			return FromIPv4HostOrder(PackOctets(bytes[0], bytes[1], bytes[2], bytes[3]));
		}
		// The scope id is deliberately not carried over: a prefix is not
		// interface-scoped, and keeping it would make the same prefix seen on
		// two interfaces into two prefixes.
		return IPv6FromOctets(bytes);
	}

	/**
	 * Textual form, or @c "<absent>" when there is no address.
	 *
	 * Defined in NetworkAddress.cpp for the reason given on FromString(): the
	 * RFC 4291 canonical form is a library's job, not this header's.
	 */
	std::string ToString() const;

	wxString ToWxString() const;

	// Comparison. See the class comment for the single rule these implement.
	//
	// All four reduce to comparing (family, octets, scope id) in that order,
	// because the storage was chosen to make them: the octets are already in
	// most-significant-first order, so std::array's lexicographic compare *is*
	// rule 3, and an IPv4 address's unused tail is always zero so it needs no
	// separate case.
	bool operator==(const CNetworkAddress &other) const noexcept
	{
		return m_family == other.m_family && m_octets == other.m_octets &&
		       m_scopeId == other.m_scopeId;
	}
	bool operator!=(const CNetworkAddress &other) const noexcept { return !(*this == other); }

	bool operator<(const CNetworkAddress &other) const noexcept
	{
		if (IsAbsent() || other.IsAbsent()) {
			// Absent sorts first; two absents are equal, so neither is less.
			return IsAbsent() && other.IsPresent();
		}
		if (m_family != other.m_family) {
			return m_family == Family::IPv4; // IPv4 before IPv6
		}
		if (m_octets != other.m_octets) {
			return m_octets < other.m_octets;
		}
		// Same octets: order by scope id so two distinct addresses never tie.
		return m_scopeId < other.m_scopeId;
	}
	bool operator>(const CNetworkAddress &other) const noexcept { return other < *this; }
	bool operator<=(const CNetworkAddress &other) const noexcept { return !(other < *this); }
	bool operator>=(const CNetworkAddress &other) const noexcept { return !(*this < other); }

	/**
	 * Reverses the four octets of a 32-bit IPv4 value, converting between the
	 * host-order and network-order conventions in either direction.
	 *
	 * Equivalent to @c wxUINT32_SWAP_ALWAYS, spelled out here so this header
	 * stays free of wxWidgets and can be unit tested on its own.
	 */
	static std::uint32_t SwapOctets(std::uint32_t value) noexcept
	{
		return ((value & 0x000000FFu) << 24) | ((value & 0x0000FF00u) << 8) |
		       ((value & 0x00FF0000u) >> 8) | ((value & 0xFF000000u) >> 24);
	}

private:
	/** Four octets, most significant first, into a host-order 32-bit value. */
	static std::uint32_t PackOctets(
		std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) noexcept
	{
		return (static_cast<std::uint32_t>(a) << 24) | (static_cast<std::uint32_t>(b) << 16) |
		       (static_cast<std::uint32_t>(c) << 8) | static_cast<std::uint32_t>(d);
	}

	/**
	 * The IPv4 address embedded in an IPv4-mapped IPv6 address, in host order.
	 * @pre IsIPv4Mapped().
	 */
	std::uint32_t EmbeddedIPv4HostOrder() const noexcept
	{
		return PackOctets(m_octets[12], m_octets[13], m_octets[14], m_octets[15]);
	}

	/**
	 * The octets, in wire order (most significant first) whatever the family.
	 *
	 * An IPv4 address occupies the first four and leaves the rest zero. That
	 * invariant is what lets the comparison operators, IsUnspecified() and
	 * TruncatedToPrefix() treat both families with one loop, so every factory
	 * above must preserve it: never write past index 3 for an IPv4 address.
	 */
	Octets m_octets{};

	//! Zero for an IPv4 or absent address; only IPv6 addresses carry a scope.
	unsigned long m_scopeId = 0;

	//! None when unset. Never conflated with the all-zero address.
	Family m_family = Family::None;
};

#endif // NETWORKADDRESS_H
// File_checked_for_headers
