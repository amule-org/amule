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

// Which address families aMule opens sockets in.
//
// Two properties are worth pinning for reasons that outlast this PR:
//
//   1. The default is IPv4-only. Piece 4 of the widening is gated so nothing advertises IPv6 until
// a switch is deliberately turned on. A default of DualStack would hand the first caller dual stack
// with no switch thrown, leaving the gate in place but guarding nothing.
//   2. Refusal never falls back. Opening a v4 socket towards a v6 target is how a truncated address
// becomes a connection to the wrong host, so a target the configuration forbids yields no protocol
// at all.
//
// The configured family is process-global mutable state, so every case below restores what it
// found. The default is captured at static-initialisation time rather than read inside a test,
// which keeps the first assertion independent of the order the cases run in.

#include <muleunit/test.h>

#include <AddressFamilyPolicyAsio.h>

using namespace muleunit;
using namespace AddressFamilyPolicy;

DECLARE_SIMPLE(AddressFamilyPolicy)

//! Read before main(), so no test can have perturbed it.
static const Families g_processDefault = Configured();

namespace
{
//! Restores the configured family, so one case cannot leak into the next.
class ScopedFamilies
{
public:
	explicit ScopedFamilies(Families families)
	: m_previous(Configured())
	{
		SetConfigured(families);
	}
	~ScopedFamilies() { SetConfigured(m_previous); }

private:
	Families m_previous;
};

CNetworkAddress Addr(const char *text)
{
	return CNetworkAddress::FromString(text);
}
} // namespace

// Absence is not the only value that names no peer. 0.0.0.0 reaches Permits() as a present IPv4
// address, so the family test alone permits it under every configuration, and a call site
// replacing an old `if (ip)` guard would dial it -- which on Linux connects to this machine.
TEST(AddressFamilyPolicy, TheUnspecifiedAddressIsNeverPermitted)
{
	const Families every[] = { Families::IPv4Only, Families::IPv6Only, Families::DualStack };
	for (const Families families : every) {
		ScopedFamilies scope(families);
		ASSERT_FALSE(Permits(Addr("0.0.0.0")));
		ASSERT_FALSE(Permits(Addr("::")));
		ASSERT_FALSE(Permits(Addr("::ffff:0.0.0.0")));
	}
	// A real address of the permitted family still passes, so this did not just refuse
	// everything.
	{
		ScopedFamilies scope(Families::DualStack);
		ASSERT_TRUE(Permits(Addr("192.0.2.1")));
		ASSERT_TRUE(Permits(Addr("2001:db8::1")));
	}
}

// The protocol and the endpoint have to be the same family. Asked separately they are not for a
// mapped target: the protocol is v4 because a mapped address is reachable over IPv4, while
// ToAsioAddress() preserves the family and yields an address_v6. Connecting those is EAFNOSUPPORT.
TEST(AddressFamilyPolicy, AsioTargetPairsTheProtocolWithAMatchingAddress)
{
	ScopedFamilies scope(Families::DualStack);

	const boost::optional<SAsioTarget> mapped = AsioTargetFor(Addr("::ffff:192.0.2.1"));
	ASSERT_TRUE((bool)mapped);
	ASSERT_TRUE(mapped->protocol == boost::asio::ip::tcp::v4());
	ASSERT_TRUE(mapped->address.is_v4());

	const boost::optional<SAsioTarget> native = AsioTargetFor(Addr("192.0.2.1"));
	ASSERT_TRUE((bool)native);
	ASSERT_TRUE(native->protocol == boost::asio::ip::tcp::v4());
	ASSERT_TRUE(native->address.is_v4());
	// The mapped and native spellings of one peer reach the same wire target.
	ASSERT_TRUE(mapped->address == native->address);

	const boost::optional<SAsioTarget> v6 = AsioTargetFor(Addr("2001:db8::1"));
	ASSERT_TRUE((bool)v6);
	ASSERT_TRUE(v6->protocol == boost::asio::ip::tcp::v6());
	ASSERT_TRUE(v6->address.is_v6());
}

// One decision, one thing to test: a refused target yields no pair at all, rather than a protocol
// the caller might dereference after a separate and possibly stale Permits() check.
TEST(AddressFamilyPolicy, AsioTargetIsEmptyForAnythingRefused)
{
	{
		ScopedFamilies scope(Families::IPv4Only);
		ASSERT_FALSE((bool)AsioTargetFor(Addr("2001:db8::1")));
	}
	{
		ScopedFamilies scope(Families::DualStack);
		ASSERT_FALSE((bool)AsioTargetFor(CNetworkAddress::Absent()));
		ASSERT_FALSE((bool)AsioTargetFor(Addr("0.0.0.0")));
	}
}

// Piece 4 will build this from a configuration integer, so a value outside the enum is reachable.
// Testing inequality against one enumerator made such a value permit both families while the
// resolver fell back to IPv4-only; every answer now agrees on the same fallback.
TEST(AddressFamilyPolicy, AnOutOfRangeConfigurationFallsBackConsistently)
{
	ScopedFamilies scope(static_cast<Families>(99));

	ASSERT_TRUE(PermitsIPv4());
	ASSERT_FALSE(PermitsIPv6());
	ASSERT_TRUE(ResolverFamilyForLookup() == ResolverFamily::IPv4Only);
	ASSERT_TRUE(Permits(Addr("192.0.2.1")));
	ASSERT_FALSE(Permits(Addr("2001:db8::1")));
}

TEST(AddressFamilyPolicy, DefaultIsIPv4Only)
{
	// Captured before any case ran. If this fails, the widening advertises a
	// family the gate was supposed to withhold.
	ASSERT_TRUE(g_processDefault == Families::IPv4Only);
}

TEST(AddressFamilyPolicy, PermitsFollowsTheConfiguredFamilies)
{
	const CNetworkAddress v4 = Addr("192.0.2.1");
	const CNetworkAddress v6 = Addr("2001:db8::1");
	{
		ScopedFamilies scope(Families::IPv4Only);
		ASSERT_TRUE(Permits(v4));
		ASSERT_FALSE(Permits(v6));
	}
	{
		ScopedFamilies scope(Families::IPv6Only);
		ASSERT_FALSE(Permits(v4));
		ASSERT_TRUE(Permits(v6));
	}
	{
		ScopedFamilies scope(Families::DualStack);
		ASSERT_TRUE(Permits(v4));
		ASSERT_TRUE(Permits(v6));
	}
}

TEST(AddressFamilyPolicy, MappedIPv4IsIPv4ForPolicy)
{
	// It narrows losslessly, so an IPv4-only configuration can reach it -- and an IPv6-only one
	// must not, or the policy would contradict IndexKey(), which collapses the two spellings to
	// one peer.
	const CNetworkAddress mapped = Addr("::ffff:192.0.2.1");
	{
		ScopedFamilies scope(Families::IPv4Only);
		ASSERT_TRUE(Permits(mapped));
	}
	{
		ScopedFamilies scope(Families::IPv6Only);
		ASSERT_FALSE(Permits(mapped));
	}
}

TEST(AddressFamilyPolicy, AbsentIsNeverPermitted)
{
	const CNetworkAddress absent = CNetworkAddress::Absent();
	ScopedFamilies scope(Families::DualStack);
	ASSERT_FALSE(Permits(absent));
	ASSERT_FALSE(TcpProtocolForTarget(absent));
}

TEST(AddressFamilyPolicy, RefusalNeverFallsBackToTheOtherFamily)
{
	const CNetworkAddress v4 = Addr("192.0.2.1");
	const CNetworkAddress v6 = Addr("2001:db8::1");
	{
		ScopedFamilies scope(Families::IPv4Only);
		ASSERT_TRUE(TcpProtocolForTarget(v4) == boost::asio::ip::tcp::v4());
		// Not v4() as a fallback: no protocol at all.
		ASSERT_FALSE(TcpProtocolForTarget(v6));
	}
	{
		ScopedFamilies scope(Families::IPv6Only);
		ASSERT_FALSE(TcpProtocolForTarget(v4));
		ASSERT_TRUE(TcpProtocolForTarget(v6) == boost::asio::ip::tcp::v6());
	}
	{
		ScopedFamilies scope(Families::DualStack);
		ASSERT_TRUE(TcpProtocolForTarget(v4) == boost::asio::ip::tcp::v4());
		ASSERT_TRUE(TcpProtocolForTarget(v6) == boost::asio::ip::tcp::v6());
		ASSERT_TRUE(TcpProtocolForTarget(Addr("::ffff:192.0.2.1")) == boost::asio::ip::tcp::v4());
	}
}

TEST(AddressFamilyPolicy, ResolverIsUnrestrictedOnlyUnderDualStack)
{
	// Any means "do not restrict the lookup", not "refuse it". That distinction
	// is why this returns its own enum rather than an optional protocol.
	{
		ScopedFamilies scope(Families::IPv4Only);
		ASSERT_TRUE(ResolverFamilyForLookup() == ResolverFamily::IPv4Only);
	}
	{
		ScopedFamilies scope(Families::IPv6Only);
		ASSERT_TRUE(ResolverFamilyForLookup() == ResolverFamily::IPv6Only);
	}
	{
		ScopedFamilies scope(Families::DualStack);
		ASSERT_TRUE(ResolverFamilyForLookup() == ResolverFamily::Any);
	}
}

TEST(AddressFamilyPolicy, AnyAddressStaysIPv4WhereverIPv4IsPermitted)
{
	// Including dual stack. Handing :: to the single-socket services -- the EC listener, the
	// web server -- would move the daemon's control channel to another family as a side effect
	// of the ed2k work.
	{
		ScopedFamilies scope(Families::IPv4Only);
		ASSERT_TRUE(AnyAddress() == AnyIPv4Address());
	}
	{
		ScopedFamilies scope(Families::DualStack);
		ASSERT_TRUE(AnyAddress() == AnyIPv4Address());
	}
	{
		ScopedFamilies scope(Families::IPv6Only);
		ASSERT_TRUE(AnyAddress() == AnyIPv6Address());
	}
}

TEST(AddressFamilyPolicy, WildcardsAreTheUnspecifiedAddresses)
{
	ASSERT_EQUALS(wxString("0.0.0.0"), wxString(AnyIPv4Address().to_string()));
	ASSERT_EQUALS(wxString("::"), wxString(AnyIPv6Address().to_string()));
}

// File_checked_for_headers
