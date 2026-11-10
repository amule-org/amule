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
#include <KadBuddyLiveness.h>

using namespace muleunit;
using Kademlia::CBuddyLiveness;

DECLARE_SIMPLE(KadBuddyLiveness)

TEST(KadBuddyLiveness, CountsPingsAndPongsUntilReset)
{
	CBuddyLiveness buddy;
	buddy.Reset(0);
	buddy.PingReceived(1000);
	buddy.PongReceived(2000);
	buddy.PongReceived(3000);
	ASSERT_EQUALS(1u, buddy.PingsReceived());
	ASSERT_EQUALS(2u, buddy.PongsReceived());
	ASSERT_EQUALS(7000u, buddy.SinceReceived(10000));
	ASSERT_EQUALS(0u, buddy.SinceReceived(2000));
	buddy.Reset(20000);
	ASSERT_EQUALS(0u, buddy.PingsReceived());
	ASSERT_EQUALS(0u, buddy.PongsReceived());
}

TEST(KadBuddyLiveness, SilentBuddyExpiresAtTheReceiveTimeout)
{
	CBuddyLiveness buddy;
	buddy.Reset(0);
	ASSERT_FALSE(buddy.Expired(CBuddyLiveness::ReceiveTimeout - 1));
	ASSERT_TRUE(buddy.Expired(CBuddyLiveness::ReceiveTimeout));
}

TEST(KadBuddyLiveness, RequestExpiresAndConnectingGivesTheFullReceiveTimeout)
{
	CBuddyLiveness buddy;
	const uint64_t start = 1000;
	buddy.Reset(start);
	ASSERT_FALSE(buddy.RequestExpired(start + CBuddyLiveness::RequestTimeout - 1));
	ASSERT_TRUE(buddy.RequestExpired(start + CBuddyLiveness::RequestTimeout));

	const uint64_t connected = start + CBuddyLiveness::RequestTimeout - 1;
	buddy.Reset(connected);
	ASSERT_FALSE(buddy.Expired(connected + CBuddyLiveness::ReceiveTimeout - 1));
	ASSERT_TRUE(buddy.Expired(connected + CBuddyLiveness::ReceiveTimeout));
}

TEST(KadBuddyLiveness, OutgoingPingsCannotKeepASilentBuddyAlive)
{
	CBuddyLiveness buddy;
	const uint64_t start = 1000;
	buddy.Reset(start);
	ASSERT_FALSE(buddy.PingDue(start + CBuddyLiveness::PingInterval - 1));
	for (uint64_t elapsed = CBuddyLiveness::PingInterval; elapsed <= 2 * CBuddyLiveness::PingInterval;
		elapsed += CBuddyLiveness::PingInterval) {
		ASSERT_TRUE(buddy.PingDue(start + elapsed));
		buddy.PingSent(start + elapsed);
		ASSERT_FALSE(buddy.Expired(start + elapsed));
	}
	ASSERT_FALSE(buddy.Expired(start + CBuddyLiveness::ReceiveTimeout - 1));
	ASSERT_TRUE(buddy.Expired(start + CBuddyLiveness::ReceiveTimeout));
}

TEST(KadBuddyLiveness, LegacyTwentyMinutePongKeepsBuddyAlive)
{
	CBuddyLiveness buddy;
	buddy.Reset(0);
	buddy.PingSent(CBuddyLiveness::PingInterval);
	const uint64_t pong = 2 * CBuddyLiveness::PingInterval;
	buddy.PingSent(pong);
	buddy.PongReceived(pong);
	ASSERT_FALSE(buddy.Expired(CBuddyLiveness::ReceiveTimeout));
	ASSERT_FALSE(buddy.PingDue(pong + CBuddyLiveness::PingInterval - 1));
	ASSERT_TRUE(buddy.PingDue(pong + CBuddyLiveness::PingInterval));
	ASSERT_TRUE(buddy.Expired(pong + CBuddyLiveness::ReceiveTimeout));
}

TEST(KadBuddyLiveness, IncomingPingThrottleDoesNotUseOutgoingSchedule)
{
	CBuddyLiveness buddy;
	buddy.Reset(0);
	ASSERT_TRUE(buddy.AllowIncomingPing(0));
	buddy.PingReceived(0);
	ASSERT_FALSE(buddy.AllowIncomingPing(CBuddyLiveness::IncomingPingInterval - 1));
	ASSERT_TRUE(buddy.AllowIncomingPing(CBuddyLiveness::IncomingPingInterval));
	buddy.PingSent(CBuddyLiveness::PingInterval);
	ASSERT_TRUE(buddy.AllowIncomingPing(CBuddyLiveness::PingInterval));
	buddy.PingReceived(CBuddyLiveness::PingInterval);
	ASSERT_FALSE(buddy.AllowIncomingPing(CBuddyLiveness::PingInterval + 1));
}

TEST(KadBuddyLiveness, ReceivedTrafficExtendsLeaseAndReconnectResetsState)
{
	CBuddyLiveness buddy;
	buddy.Reset(0);
	buddy.ReceivedPacket(CBuddyLiveness::ReceiveTimeout - 1);
	ASSERT_FALSE(buddy.Expired(CBuddyLiveness::ReceiveTimeout));
	ASSERT_TRUE(buddy.Expired(2 * CBuddyLiveness::ReceiveTimeout - 1));
	buddy.PingReceived(2 * CBuddyLiveness::ReceiveTimeout);
	buddy.Reset(2 * CBuddyLiveness::ReceiveTimeout + 1);
	ASSERT_TRUE(buddy.AllowIncomingPing(2 * CBuddyLiveness::ReceiveTimeout + 1));
	ASSERT_FALSE(buddy.Expired(2 * CBuddyLiveness::ReceiveTimeout + 1));
}

TEST(KadBuddyLiveness, BothEndsStayAliveThenReleaseTheSilentConnection)
{
	CBuddyLiveness client;
	CBuddyLiveness host;
	client.Reset(0);
	host.Reset(0);
	uint64_t now = 0;
	for (unsigned int ping = 0; ping < 6; ++ping) {
		now += CBuddyLiveness::PingInterval;
		ASSERT_TRUE(client.PingDue(now));
		client.PingSent(now);
		ASSERT_TRUE(host.AllowIncomingPing(now));
		host.PingReceived(now);
		client.PongReceived(now);
		ASSERT_FALSE(client.Expired(now));
		ASSERT_FALSE(host.Expired(now));
	}
	// Neither local sends nor the absence of an explicit TCP close keep the lease alive.
	client.PingSent(now + CBuddyLiveness::PingInterval);
	client.PingSent(now + 2 * CBuddyLiveness::PingInterval);
	ASSERT_TRUE(client.Expired(now + CBuddyLiveness::ReceiveTimeout));
	ASSERT_TRUE(host.Expired(now + CBuddyLiveness::ReceiveTimeout));
}
