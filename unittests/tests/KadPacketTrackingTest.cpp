//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
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
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
//

#include <muleunit/test.h>
#include <kademlia/net/PacketTracking.h>
#include <kademlia/kademlia/Kademlia.h>
#include <GetTickCount.h>
#include <protocol/kad2/Client2Client/UDP.h>
#include <protocol/kad/Client2Client/UDP.h>

using namespace muleunit;
using namespace Kademlia;
DECLARE_SIMPLE(KadPacketTracking)
namespace
{
uint64_t now = 0;
bool lan = false;
} // namespace
uint64 GetTickCount64()
{
	return now;
}
bool CKademlia::IsRunningInLANMode()
{
	return lan;
}
bool IsLanIP(uint32_t ip) noexcept
{
	return (ip & 0xff) == 10;
}

class Tracker : public CPacketTracking
{
public:
	using CPacketTracking::AddLegacyChallenge;
	using CPacketTracking::AddTrackedOutPacket;
	using CPacketTracking::HasActiveLegacyChallenge;
	using CPacketTracking::InTrackListAllowedPerMinute;
	using CPacketTracking::InTrackListCount;
	using CPacketTracking::InTrackListIsAllowedPacket;
	using CPacketTracking::IsLegacyChallenge;
	using CPacketTracking::IsOnOutTrackList;
};
using Result = CPacketTracking::IncomingPacket;

TEST(KadPacketTracking, CapacityRejectsNewAddressesWithoutResettingLiveQuotas)
{
	now = 1000;
	lan = false;
	Tracker tracker;
	for (uint32_t ip = 1; ip <= 10000; ++ip) {
		ASSERT_TRUE(tracker.InTrackListIsAllowedPacket(ip, KADEMLIA2_PING, false) == Result::Allowed);
	}
	ASSERT_TRUE(tracker.InTrackListIsAllowedPacket(10001, KADEMLIA2_PING, false) == Result::Drop);
	ASSERT_TRUE(tracker.InTrackListIsAllowedPacket(1, KADEMLIA2_PING, false) == Result::Allowed);
	ASSERT_TRUE(tracker.InTrackListIsAllowedPacket(1, KADEMLIA2_PING, false) == Result::Drop);
	// The response path remains usable even when request tracking is full.
	ASSERT_TRUE(tracker.InTrackListIsAllowedPacket(10001, KADEMLIA2_PONG, false) == Result::Allowed);
	// Expired entries are cleaned early, well before the 12-minute cleanup.
	now += 2 * 60000;
	ASSERT_TRUE(tracker.InTrackListIsAllowedPacket(10001, KADEMLIA2_PING, false) == Result::Allowed);
	ASSERT_TRUE(tracker.InTrackListIsAllowedPacket(1, KADEMLIA2_PING, false) == Result::Allowed);
}

TEST(KadPacketTracking, CountReadsTheLimiterWithoutChangingIt)
{
	now = 1000;
	lan = false;
	Tracker tracker;
	ASSERT_EQUALS(2u, Tracker::InTrackListAllowedPerMinute(KADEMLIA_FINDBUDDY_REQ));
	ASSERT_EQUALS(2u, Tracker::InTrackListAllowedPerMinute(KADEMLIA_FIREWALLED2_REQ));
	ASSERT_EQUALS(0u, Tracker::InTrackListAllowedPerMinute(KADEMLIA2_PONG));
	ASSERT_EQUALS(0u, tracker.InTrackListCount(1, KADEMLIA_FINDBUDDY_REQ));

	// Two a minute: one every 30 s drains.
	ASSERT_TRUE(tracker.InTrackListIsAllowedPacket(1, KADEMLIA_FINDBUDDY_REQ, false) == Result::Allowed);
	ASSERT_TRUE(tracker.InTrackListIsAllowedPacket(1, KADEMLIA_FINDBUDDY_REQ, false) == Result::Allowed);
	ASSERT_EQUALS(2u, tracker.InTrackListCount(1, KADEMLIA_FINDBUDDY_REQ));
	now += 29999;
	ASSERT_TRUE(tracker.InTrackListIsAllowedPacket(1, KADEMLIA_FINDBUDDY_REQ, false) == Result::Drop);
	ASSERT_EQUALS(3u, tracker.InTrackListCount(1, KADEMLIA_FINDBUDDY_REQ));
	// Reading twice changes nothing; the count drains one per interval.
	ASSERT_EQUALS(3u, tracker.InTrackListCount(1, KADEMLIA_FINDBUDDY_REQ));
	now += 1;
	ASSERT_EQUALS(2u, tracker.InTrackListCount(1, KADEMLIA_FINDBUDDY_REQ));
	now += 60000;
	ASSERT_EQUALS(0u, tracker.InTrackListCount(1, KADEMLIA_FINDBUDDY_REQ));
	// Another opcode from the same sender has its own count, and the two firewall requests share one.
	ASSERT_EQUALS(0u, tracker.InTrackListCount(1, KADEMLIA_CALLBACK_REQ));
	ASSERT_TRUE(
		tracker.InTrackListIsAllowedPacket(1, KADEMLIA_FIREWALLED2_REQ, false) == Result::Allowed);
	ASSERT_EQUALS(1u, tracker.InTrackListCount(1, KADEMLIA_FIREWALLED_REQ));
}

// The per-sender ceiling for callback requests is a flood limit only: each served buddy enforces one
// a minute per sender, so a caller may reach every buddy we serve in the same minute.
TEST(KadPacketTracking, CallbackRequestsAllowOnePerServedBuddy)
{
	now = 1000;
	lan = false;
	Tracker tracker;
	ASSERT_EQUALS(16u, Tracker::InTrackListAllowedPerMinute(KADEMLIA_CALLBACK_REQ));
	for (unsigned i = 0; i < 16; ++i) {
		ASSERT_TRUE(tracker.InTrackListIsAllowedPacket(1, KADEMLIA_CALLBACK_REQ, false) ==
			    Result::Allowed);
	}
	ASSERT_TRUE(tracker.InTrackListIsAllowedPacket(1, KADEMLIA_CALLBACK_REQ, false) == Result::Drop);
}

TEST(KadPacketTracking, CountIsZeroForASenderDroppedByAFullTable)
{
	now = 1000;
	lan = false;
	Tracker tracker;
	for (uint32_t ip = 1; ip <= 10000; ++ip) {
		ASSERT_TRUE(tracker.InTrackListIsAllowedPacket(ip, KADEMLIA2_PING, false) == Result::Allowed);
	}
	ASSERT_TRUE(tracker.InTrackListIsAllowedPacket(10001, KADEMLIA_CALLBACK_REQ, false) == Result::Drop);
	ASSERT_EQUALS(0u, tracker.InTrackListCount(10001, KADEMLIA_CALLBACK_REQ));
}

TEST(KadPacketTracking, SpoofedTrafficCannotPrimeTheVerifiedBanBudget)
{
	now = 1000;
	lan = false;
	Tracker tracker;
	for (unsigned i = 0; i < 100000; ++i) {
		ASSERT_FALSE(
			tracker.InTrackListIsAllowedPacket(1, KADEMLIA2_PING, false) == Result::MassiveFlood);
	}
	for (unsigned i = 0; i < 10; ++i) {
		ASSERT_TRUE(tracker.InTrackListIsAllowedPacket(1, KADEMLIA2_PING, true) == Result::Drop);
	}
	ASSERT_TRUE(tracker.InTrackListIsAllowedPacket(1, KADEMLIA2_PING, true) == Result::MassiveFlood);
	// Even extreme floods expire predictably once traffic stops.
	now += 6 * 60000;
	ASSERT_TRUE(tracker.InTrackListIsAllowedPacket(1, KADEMLIA2_PING, true) == Result::Allowed);
}

TEST(KadPacketTracking, OutgoingTrackingKeepsRecentEntriesWithinItsBound)
{
	now = 1000;
	Tracker tracker;
	for (uint32_t ip = 1; ip <= 4096 + 1; ++ip) {
		tracker.AddTrackedOutPacket(ip, KADEMLIA2_PING);
	}
	ASSERT_FALSE(tracker.IsOnOutTrackList(1, KADEMLIA2_PING));
	ASSERT_TRUE(tracker.IsOnOutTrackList(2, KADEMLIA2_PING, true));
	ASSERT_TRUE(tracker.IsOnOutTrackList(2, KADEMLIA2_PING));
	ASSERT_FALSE(tracker.IsOnOutTrackList(2, KADEMLIA2_PING));

	now += 180000;
	ASSERT_FALSE(tracker.IsOnOutTrackList(3, KADEMLIA2_PING));
}
