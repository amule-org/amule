// This file is part of the aMule Project.
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Licensed under the GNU General Public License, version 2 or later.

#include <muleunit/test.h>
#include <kademlia/kademlia/PublicIPVote.h>

using namespace muleunit;
using Kademlia::CPublicIPVote;

DECLARE_SIMPLE(KadPublicIPVote)

namespace
{
const uint32_t kOurIP = 0x5d000001;
const uint32_t kFakeIP = 0x01020304;
const uint32_t kPeerA = 0x0a000001;
const uint32_t kPeerASameNet = 0x0a0000fe;
const uint32_t kPeerB = 0x0b000001;
} // namespace

TEST(KadPublicIPVote, FirstReportIsTaken)
{
	CPublicIPVote vote;
	vote.Report(kOurIP, kPeerA);
	ASSERT_EQUALS(kOurIP, vote.Get());
}

TEST(KadPublicIPVote, ChangeNeedsTwoReportersFromDifferentSubnets)
{
	CPublicIPVote vote;
	vote.Report(kOurIP, kPeerA);
	vote.Report(kFakeIP, kPeerA);
	vote.Report(kFakeIP, kPeerA);
	vote.Report(kFakeIP, kPeerASameNet);
	ASSERT_EQUALS(kOurIP, vote.Get());
	vote.Report(kFakeIP, kPeerB);
	ASSERT_EQUALS(kFakeIP, vote.Get());
}

TEST(KadPublicIPVote, MismatchRestartsTheVote)
{
	CPublicIPVote vote;
	vote.Report(kOurIP, kPeerA);
	vote.Report(kFakeIP, kPeerA);
	vote.Report(kOurIP + 1, kPeerB);
	vote.Report(kFakeIP, kPeerB);
	ASSERT_EQUALS(kOurIP, vote.Get());
}

TEST(KadPublicIPVote, LoadedIPIsNobodysReport)
{
	CPublicIPVote vote;
	vote.Load(kOurIP);
	ASSERT_EQUALS(kOurIP, vote.Get());
	vote.Report(kFakeIP, kPeerA);
	ASSERT_EQUALS(kFakeIP, vote.Get());
}
