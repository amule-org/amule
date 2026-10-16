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

#include <muleunit/test.h>
#include "AdvertisedIPv6.h"
#include "IPv6ChangeNotification.h"

using namespace muleunit;

DECLARE_SIMPLE(AdvertisedIPv6)

namespace
{
typedef std::array<std::uint8_t, 16> Bytes;

// 2a00:1450:4001:80b::<suffix>, a globally routable prefix outside every excluded range.
Bytes Global(std::uint8_t suffix)
{
	return { 0x2a, 0x00, 0x14, 0x50, 0x40, 0x01, 0x08, 0x0b, 0, 0, 0, 0, 0, 0, 0, suffix };
}

NetworkInterfaceIPv6 Address(const Bytes &bytes, bool temporary = false, bool deprecated = false)
{
	NetworkInterfaceIPv6 address;
	address.bytes = bytes;
	address.temporary = temporary;
	address.deprecated = deprecated;
	return address;
}

NetworkInterface Interface(const wxString &name, std::vector<NetworkInterfaceIPv6> addresses)
{
	NetworkInterface iface;
	iface.name = name;
	iface.ipv6 = std::move(addresses);
	return iface;
}

bool Chooses(const std::vector<NetworkInterface> &interfaces,
	const Bytes &expected,
	const wxString &pinned = wxEmptyString)
{
	const std::optional<CNetworkAddress> chosen = ChooseAdvertisedIPv6(interfaces, pinned);
	return chosen.has_value() && *chosen == CNetworkAddress::IPv6FromOctets(expected);
}

SAdvertisedIPv6Facts AdvertisableFacts()
{
	SAdvertisedIPv6Facts facts;
	facts.choice = CNetworkAddress::IPv6FromOctets(Global(0x0a));
	facts.declaredReachable = true;
	facts.ipv6Built = true;
	facts.familyPermitted = true;
	facts.proxyEnabled = false;
	facts.listenerAcceptsIPv6 = true;
	return facts;
}
} // namespace

TEST(AdvertisedIPv6, NothingIsChosenWithoutAPublishableAddress)
{
	ASSERT_FALSE(ChooseAdvertisedIPv6({}, wxEmptyString).has_value());

	const Bytes uniqueLocal = { 0xfd, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
	const Bytes linkLocal = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
	const Bytes documentation = { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
	const Bytes sixToFour = { 0x20, 0x02, 0xc0, 0x00, 0x02, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
	const Bytes teredo = { 0x20, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
	const std::vector<NetworkInterface> interfaces = { Interface("eth0",
		{ Address(uniqueLocal),
			Address(linkLocal),
			Address(documentation),
			Address(sixToFour),
			Address(teredo) }) };
	ASSERT_FALSE(ChooseAdvertisedIPv6(interfaces, wxEmptyString).has_value());
}

TEST(AdvertisedIPv6, AStableAddressIsPreferredOverATemporaryOne)
{
	// The temporary address sorts first, so only the preference can pick the stable one.
	const std::vector<NetworkInterface> interfaces = { Interface(
		"eth0", { Address(Global(1), true), Address(Global(2)) }) };
	ASSERT_TRUE(Chooses(interfaces, Global(2)));
}

TEST(AdvertisedIPv6, ATemporaryAddressIsChosenWhenNothingElseIs)
{
	const std::vector<NetworkInterface> interfaces = { Interface("eth0", { Address(Global(1), true) }) };
	ASSERT_TRUE(Chooses(interfaces, Global(1)));
}

TEST(AdvertisedIPv6, DeprecatedAddressesAreNeverChosen)
{
	const std::vector<NetworkInterface> onlyDeprecated = { Interface(
		"eth0", { Address(Global(1), false, true) }) };
	ASSERT_FALSE(ChooseAdvertisedIPv6(onlyDeprecated, wxEmptyString).has_value());

	const std::vector<NetworkInterface> interfaces = { Interface(
		"eth0", { Address(Global(1), false, true), Address(Global(2), true) }) };
	ASSERT_TRUE(Chooses(interfaces, Global(2)));
}

// Inbound traffic arrives only through a pinned interface, so only its addresses can be offered.
TEST(AdvertisedIPv6, APinnedInterfaceLimitsTheChoice)
{
	const std::vector<NetworkInterface> interfaces = { Interface("eth0", { Address(Global(1)) }),
		Interface("wlan0", { Address(Global(2)) }) };
	ASSERT_TRUE(Chooses(interfaces, Global(2), "wlan0"));
	ASSERT_FALSE(ChooseAdvertisedIPv6(interfaces, "tun0").has_value());
}

// Every refresh must agree on the answer, whatever order the platform enumerates in.
TEST(AdvertisedIPv6, TheLowestAddressWinsATie)
{
	const std::vector<NetworkInterface> interfaces = { Interface("eth0", { Address(Global(9)) }),
		Interface("wlan0", { Address(Global(3)), Address(Global(5)) }) };
	ASSERT_TRUE(Chooses(interfaces, Global(3)));
}

TEST(AdvertisedIPv6, TheChoiceIsAdvertisedWhenEveryConditionHolds)
{
	const std::optional<CNetworkAddress> advertised = DecideAdvertisedIPv6(AdvertisableFacts());
	ASSERT_TRUE(advertised.has_value());
	ASSERT_TRUE(*advertised == CNetworkAddress::IPv6FromOctets(Global(0x0a)));
}

TEST(AdvertisedIPv6, NothingIsAdvertisedWithoutAChoice)
{
	SAdvertisedIPv6Facts facts = AdvertisableFacts();
	facts.choice = std::nullopt;
	ASSERT_FALSE(DecideAdvertisedIPv6(facts).has_value());
}

TEST(AdvertisedIPv6, NothingIsAdvertisedWhenPeersContradictThePrefix)
{
	SAdvertisedIPv6Facts facts = AdvertisableFacts();
	facts.contradicted = true;
	ASSERT_FALSE(DecideAdvertisedIPv6(facts).has_value());
}

TEST(AdvertisedIPv6, NothingIsAdvertisedUnlessTheUserDeclaresThePortReachable)
{
	SAdvertisedIPv6Facts facts = AdvertisableFacts();
	facts.declaredReachable = false;
	ASSERT_FALSE(DecideAdvertisedIPv6(facts).has_value());
}

TEST(AdvertisedIPv6, NothingIsAdvertisedWithoutIPv6Support)
{
	SAdvertisedIPv6Facts facts = AdvertisableFacts();
	facts.ipv6Built = false;
	ASSERT_FALSE(DecideAdvertisedIPv6(facts).has_value());
}

TEST(AdvertisedIPv6, NothingIsAdvertisedWhenTheFamilyIsNotPermitted)
{
	SAdvertisedIPv6Facts facts = AdvertisableFacts();
	facts.familyPermitted = false;
	ASSERT_FALSE(DecideAdvertisedIPv6(facts).has_value());
}

TEST(AdvertisedIPv6, NothingIsAdvertisedWithAProxyConfigured)
{
	SAdvertisedIPv6Facts facts = AdvertisableFacts();
	facts.proxyEnabled = true;
	ASSERT_FALSE(DecideAdvertisedIPv6(facts).has_value());
}

TEST(AdvertisedIPv6, NothingIsAdvertisedWhenTheListenerRejectsIPv6)
{
	SAdvertisedIPv6Facts facts = AdvertisableFacts();
	facts.listenerAcceptsIPv6 = false;
	ASSERT_FALSE(DecideAdvertisedIPv6(facts).has_value());
}

TEST(AdvertisedIPv6, KadSourcesRepublishOnlyWhenTheirPublishedAddressBecomesWrong)
{
	const std::optional<CNetworkAddress> published = CNetworkAddress::IPv6FromOctets(Global(0x0a));

	SAdvertisedIPv6Facts restored = AdvertisableFacts();
	ASSERT_FALSE(ShouldRepublishKadSources(published, restored));

	SAdvertisedIPv6Facts changed = AdvertisableFacts();
	changed.choice = CNetworkAddress::IPv6FromOctets(Global(0x0b));
	ASSERT_TRUE(ShouldRepublishKadSources(published, changed));

	SAdvertisedIPv6Facts disabled = AdvertisableFacts();
	disabled.declaredReachable = false;
	ASSERT_TRUE(ShouldRepublishKadSources(published, disabled));
}

// Peers dial what Kad holds, so an address the outside world cannot reach has to leave it.
TEST(AdvertisedIPv6, KadSourcesRepublishWhenAVetoWithdrawsTheAddress)
{
	const std::optional<CNetworkAddress> published = CNetworkAddress::IPv6FromOctets(Global(0x0a));

	SAdvertisedIPv6Facts vetoed = AdvertisableFacts();
	vetoed.contradicted = true;
	ASSERT_TRUE(ShouldRepublishKadSources(published, vetoed));
}

// OP_CHANGE_CLIENT_IP tells peers to dial a new address, so a veto must never send one, and the
// same address returning after a veto lifts is nothing new to tell peers that already hold it.
TEST(AdvertisedIPv6, AVetoSendsNoChangeNotification)
{
	const std::optional<CNetworkAddress> notified = DecideAdvertisedIPv6(AdvertisableFacts());
	ASSERT_TRUE(notified.has_value());

	SAdvertisedIPv6Facts vetoed = AdvertisableFacts();
	vetoed.contradicted = true;
	ASSERT_FALSE(ShouldMarkIPv6ChangeNotification(notified, DecideAdvertisedIPv6(vetoed)));
	ASSERT_FALSE(ShouldMarkIPv6ChangeNotification(notified, DecideAdvertisedIPv6(AdvertisableFacts())));
}

TEST(AdvertisedIPv6, KadSourcesRepublishForTheFirstAdvertisement)
{
	ASSERT_TRUE(ShouldRepublishKadSources(std::nullopt, AdvertisableFacts()));
}

TEST(AdvertisedIPv6, NothingPublishedNeedsNoRepublish)
{
	SAdvertisedIPv6Facts vetoed = AdvertisableFacts();
	vetoed.contradicted = true;
	ASSERT_FALSE(ShouldRepublishKadSources(std::nullopt, vetoed));

	SAdvertisedIPv6Facts disabled = AdvertisableFacts();
	disabled.declaredReachable = false;
	ASSERT_FALSE(ShouldRepublishKadSources(std::nullopt, disabled));
}
