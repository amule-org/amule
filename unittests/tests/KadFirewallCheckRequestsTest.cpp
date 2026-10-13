// This file is part of the aMule Project.
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Licensed under the GNU General Public License, version 2 or later.

#include <muleunit/test.h>
#include <kademlia/kademlia/FirewallCheckRequests.h>

using namespace muleunit;
using Kademlia::CFirewallCheckRequests;

DECLARE_SIMPLE(KadFirewallCheckRequests)

namespace
{
const uint64_t kStart = 1000000;
const uint32_t kPeer = 0x0a000001;
const uint32_t kOther = 0x0a000002;
const auto kIP = CFirewallCheckRequests::Reply::PublicIP;
const auto kReachable = CFirewallCheckRequests::Reply::Reachable;
} // namespace

TEST(KadFirewallCheckRequests, UnaskedPeerGetsNothing)
{
	CFirewallCheckRequests requests;
	requests.Add(kPeer, kStart);
	ASSERT_FALSE(requests.Contains(kOther, kStart));
	ASSERT_FALSE(requests.Take(kOther, kIP, kStart));
	ASSERT_FALSE(requests.Take(kOther, kReachable, kStart));
}

TEST(KadFirewallCheckRequests, EachReplyKindOncePerRequest)
{
	CFirewallCheckRequests requests;
	requests.Add(kPeer, kStart);
	ASSERT_TRUE(requests.Take(kPeer, kIP, kStart + 1));
	ASSERT_FALSE(requests.Take(kPeer, kIP, kStart + 2));
	ASSERT_TRUE(requests.Take(kPeer, kReachable, kStart + 3));
	ASSERT_FALSE(requests.Take(kPeer, kReachable, kStart + 4));
	// Still listed: the buddy and callback paths only ask whether a check is running.
	ASSERT_TRUE(requests.Contains(kPeer, kStart + 5));
}

TEST(KadFirewallCheckRequests, SecondRequestAllowsSecondReply)
{
	CFirewallCheckRequests requests;
	requests.Add(kPeer, kStart);
	requests.Add(kPeer, kStart + 10);
	ASSERT_TRUE(requests.Take(kPeer, kReachable, kStart + 20));
	ASSERT_TRUE(requests.Take(kPeer, kReachable, kStart + 21));
	ASSERT_FALSE(requests.Take(kPeer, kReachable, kStart + 22));
}

TEST(KadFirewallCheckRequests, RequestsExpire)
{
	CFirewallCheckRequests requests;
	requests.Add(kPeer, kStart);
	const uint64_t expired = kStart + CFirewallCheckRequests::kLifetimeMs;
	ASSERT_FALSE(requests.Contains(kPeer, expired));
	ASSERT_FALSE(requests.Take(kPeer, kIP, expired));
	// Adding later drops the stale request, so a new one is the only one left.
	requests.Add(kOther, expired + 1);
	requests.Add(kPeer, expired + 2);
	ASSERT_TRUE(requests.Take(kPeer, kIP, expired + 3));
	ASSERT_FALSE(requests.Take(kPeer, kIP, expired + 4));
}
