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
#include "PublicIPAnswerPolicy.h"

using namespace muleunit;

DECLARE_SIMPLE(PublicIPAnswerPolicy)

namespace
{
//! An ed2k-order IPv4, the form OP_PUBLICIP_ANSWER (0x98) carries.
constexpr std::uint32_t Ip(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d)
{
	return a | (b << 8) | (c << 16) | (d << 24);
}

const std::uint32_t kOurIP = Ip(93, 184, 216, 34);
const std::uint32_t kWrongIP = Ip(81, 2, 3, 4);
const std::uint32_t kPeerA = Ip(6, 61, 0, 5);
const std::uint32_t kPeerASameNet = Ip(6, 61, 200, 9);
const std::uint32_t kPeerB = Ip(7, 62, 0, 5);
const std::uint32_t kPeerC = Ip(8, 63, 0, 5);
} // namespace

TEST(PublicIPAnswerPolicy, OneAnswerIsNotEnough)
{
	CPublicIPAnswerQuorum votes;
	ASSERT_EQUALS(0u, votes.Add(kPeerA, kOurIP));
	ASSERT_EQUALS(1u, votes.VoteCount());
}

TEST(PublicIPAnswerPolicy, OneSlash16IsOneVote)
{
	CPublicIPAnswerQuorum votes;
	ASSERT_EQUALS(0u, votes.Add(kPeerA, kOurIP));
	ASSERT_EQUALS(0u, votes.Add(kPeerASameNet, kOurIP));
	ASSERT_EQUALS(0u, votes.Add(kPeerA, kOurIP));
	ASSERT_EQUALS(1u, votes.VoteCount());
}

TEST(PublicIPAnswerPolicy, TwoSlash16sThatAgreeAreAdopted)
{
	CPublicIPAnswerQuorum votes;
	ASSERT_EQUALS(0u, votes.Add(kPeerA, kOurIP));
	ASSERT_EQUALS(kOurIP, votes.Add(kPeerB, kOurIP));
	ASSERT_EQUALS(0u, votes.VoteCount());
}

TEST(PublicIPAnswerPolicy, DisagreementAdoptsNothing)
{
	CPublicIPAnswerQuorum votes;
	ASSERT_EQUALS(0u, votes.Add(kPeerA, kWrongIP));
	ASSERT_EQUALS(0u, votes.Add(kPeerB, kOurIP));
	ASSERT_EQUALS(2u, votes.VoteCount());
}

TEST(PublicIPAnswerPolicy, OneWrongPeerDoesNotBlockTwoThatAgree)
{
	CPublicIPAnswerQuorum votes;
	votes.Add(kPeerA, kWrongIP);
	votes.Add(kPeerB, kOurIP);
	ASSERT_EQUALS(kOurIP, votes.Add(kPeerC, kOurIP));
}

TEST(PublicIPAnswerPolicy, ASlash16OnlyKeepsItsLatestAnswer)
{
	CPublicIPAnswerQuorum votes;
	votes.Add(kPeerA, kOurIP);
	votes.Add(kPeerASameNet, kWrongIP);
	ASSERT_EQUALS(0u, votes.Add(kPeerB, kOurIP));
	ASSERT_EQUALS(kWrongIP, votes.Add(kPeerC, kWrongIP));
}

TEST(PublicIPAnswerPolicy, ResetForgetsTheVotes)
{
	CPublicIPAnswerQuorum votes;
	votes.Add(kPeerA, kOurIP);
	votes.Reset();
	ASSERT_EQUALS(0u, votes.VoteCount());
	ASSERT_EQUALS(0u, votes.Add(kPeerB, kOurIP));
}

TEST(PublicIPAnswerPolicy, LowIDValueDoesNotVote)
{
	const std::uint32_t dotZero = Ip(93, 184, 216, 0);
	ASSERT_TRUE(IsLowID(dotZero));
	CPublicIPAnswerQuorum votes;
	ASSERT_EQUALS(0u, votes.Add(kPeerA, dotZero));
	ASSERT_EQUALS(0u, votes.Add(kPeerB, dotZero));
	ASSERT_EQUALS(0u, votes.Add(kPeerC, 0));
	ASSERT_EQUALS(0u, votes.VoteCount());
}

TEST(PublicIPAnswerPolicy, NonPublicValueDoesNotVote)
{
	CPublicIPAnswerQuorum votes;
	const std::uint32_t nonPublic[] = {
		Ip(192, 168, 1, 5), Ip(10, 0, 0, 5), Ip(100, 64, 0, 5), Ip(127, 0, 1, 1), Ip(169, 254, 1, 1)
	};
	for (const std::uint32_t value : nonPublic) {
		ASSERT_EQUALS(0u, votes.Add(kPeerA, value));
		ASSERT_EQUALS(0u, votes.Add(kPeerB, value));
	}
	ASSERT_EQUALS(0u, votes.VoteCount());
}

TEST(PublicIPAnswerPolicy, NonPublicAnswererDoesNotVote)
{
	CPublicIPAnswerQuorum votes;
	ASSERT_EQUALS(0u, votes.Add(Ip(192, 168, 1, 5), kOurIP));
	ASSERT_EQUALS(0u, votes.Add(Ip(10, 1, 0, 5), kOurIP));
	ASSERT_EQUALS(0u, votes.Add(Ip(172, 17, 0, 2), kOurIP));
	ASSERT_EQUALS(0u, votes.Add(Ip(100, 64, 0, 9), kOurIP));
	ASSERT_EQUALS(0u, votes.VoteCount());
	ASSERT_EQUALS(0u, votes.Add(kPeerA, kOurIP));
}

TEST(PublicIPAnswerPolicy, AFullListDropsTheOldestVote)
{
	CPublicIPAnswerQuorum votes;
	votes.Add(Ip(20, 0, 0, 1), kOurIP);
	for (std::uint32_t net = 1; net < PUBLIC_IP_ANSWER_MAX_VOTES; ++net) {
		votes.Add(Ip(20, net, 0, 1), Ip(81, 2, 3, net));
	}
	ASSERT_EQUALS(PUBLIC_IP_ANSWER_MAX_VOTES, votes.VoteCount());
	votes.Add(Ip(21, 0, 0, 1), Ip(82, 2, 3, 4));
	ASSERT_EQUALS(PUBLIC_IP_ANSWER_MAX_VOTES, votes.VoteCount());
	ASSERT_EQUALS(0u, votes.Add(Ip(22, 0, 0, 1), kOurIP));
	ASSERT_EQUALS(kOurIP, votes.Add(Ip(23, 0, 0, 1), kOurIP));
}
