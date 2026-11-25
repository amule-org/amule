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
#include "ServerIPv6Callback.h"

#include <vector>

using namespace muleunit;

DECLARE_SIMPLE(ServerIPv6Callback)

namespace
{
std::vector<std::uint8_t> Request(std::size_t size)
{
	std::vector<std::uint8_t> data(size);
	for (std::size_t i = 0; i < size; ++i) {
		data[i] = static_cast<std::uint8_t>(i + 1);
	}
	return data;
}

SIPv6CallbackRequestFacts Askable()
{
	SIPv6CallbackRequestFacts facts;
	facts.weAreLowID = true;
	facts.sourceLowID = true;
	facts.sourceOnOurServer = true;
	facts.serverOffers = true;
	facts.verdict = EServerIPv6Verdict::Verified;
	return facts;
}
} // namespace

TEST(ServerIPv6Callback, TheShortFormIsAddressAndPort)
{
	const std::vector<std::uint8_t> data = Request(18);
	const std::optional<SIPv6CallbackRequested> parsed =
		ParseIPv6CallbackRequested(data.data(), data.size());
	ASSERT_TRUE(parsed.has_value());
	ASSERT_EQUALS(1, parsed->address[0]);
	ASSERT_EQUALS(16, parsed->address[15]);
	// Little-endian, as in OP_CALLBACKREQUESTED.
	ASSERT_EQUALS(0x1211, parsed->port);
	ASSERT_FALSE(parsed->cryptOptions.has_value());
	ASSERT_FALSE(parsed->userHash.has_value());
}

TEST(ServerIPv6Callback, TheLongFormAddsTheCryptPart)
{
	const std::vector<std::uint8_t> data = Request(35);
	const std::optional<SIPv6CallbackRequested> parsed =
		ParseIPv6CallbackRequested(data.data(), data.size());
	ASSERT_TRUE(parsed.has_value());
	ASSERT_EQUALS(19, *parsed->cryptOptions);
	ASSERT_EQUALS(20, (*parsed->userHash)[0]);
	ASSERT_EQUALS(35, (*parsed->userHash)[15]);
}

TEST(ServerIPv6Callback, ATruncatedRequestIsDroppedAndAPartialCryptPartIgnored)
{
	const std::vector<std::uint8_t> data = Request(34);
	ASSERT_FALSE(ParseIPv6CallbackRequested(data.data(), 17).has_value());
	const std::optional<SIPv6CallbackRequested> parsed = ParseIPv6CallbackRequested(data.data(), 34);
	ASSERT_TRUE(parsed.has_value());
	ASSERT_FALSE(parsed->cryptOptions.has_value());
}

TEST(ServerIPv6Callback, OnlyAVerifiedIPv6OnAnOfferingServerIsAsked)
{
	ASSERT_TRUE(ShouldRequestIPv6Callback(Askable()));
	for (EServerIPv6Verdict verdict : { EServerIPv6Verdict::Unknown,
		     EServerIPv6Verdict::Probing,
		     EServerIPv6Verdict::Trusted,
		     EServerIPv6Verdict::Failed }) {
		SIPv6CallbackRequestFacts facts = Askable();
		facts.verdict = verdict;
		ASSERT_FALSE(ShouldRequestIPv6Callback(facts));
	}
	SIPv6CallbackRequestFacts facts = Askable();
	facts.serverOffers = false;
	ASSERT_FALSE(ShouldRequestIPv6Callback(facts));
	facts = Askable();
	facts.weAreLowID = false;
	ASSERT_FALSE(ShouldRequestIPv6Callback(facts));
	facts = Askable();
	facts.sourceLowID = false;
	ASSERT_FALSE(ShouldRequestIPv6Callback(facts));
	facts = Askable();
	facts.sourceOnOurServer = false;
	ASSERT_FALSE(ShouldRequestIPv6Callback(facts));
}

TEST(ServerIPv6Callback, TheOptInGoesOnceAndOnlyWhereWeCanDial)
{
	ASSERT_TRUE(ShouldOptInToIPv6Callback(true, false, true, true));
	ASSERT_FALSE(ShouldOptInToIPv6Callback(false, false, true, true));
	ASSERT_FALSE(ShouldOptInToIPv6Callback(true, true, true, true));
	ASSERT_FALSE(ShouldOptInToIPv6Callback(true, false, false, true));
	ASSERT_FALSE(ShouldOptInToIPv6Callback(true, false, true, false));
}

TEST(ServerIPv6Callback, ARetryWaitsForItsTimeAndForANatStepsWindow)
{
	ASSERT_TRUE(IsIPv6CallbackRetryDue(100, 100, false, 0));
	ASSERT_FALSE(IsIPv6CallbackRetryDue(99, 100, false, 0));
	ASSERT_FALSE(IsIPv6CallbackRetryDue(100, 0, true, kNatStepAnswerWindowMs - 1));
	ASSERT_TRUE(IsIPv6CallbackRetryDue(100, 0, true, kNatStepAnswerWindowMs));
}

TEST(ServerIPv6Callback, ALowIDSourceIsKeptWhileTheCallbackCanReachIt)
{
	ASSERT_TRUE(ShouldKeepLowIdSourceForIPv6Callback(true, true, true, EServerIPv6Verdict::Verified));
	// The first source answers come before the dial-back's verdict.
	ASSERT_TRUE(ShouldKeepLowIdSourceForIPv6Callback(true, true, true, EServerIPv6Verdict::Probing));
	ASSERT_FALSE(ShouldKeepLowIdSourceForIPv6Callback(true, true, true, EServerIPv6Verdict::Failed));
	ASSERT_FALSE(ShouldKeepLowIdSourceForIPv6Callback(true, true, true, EServerIPv6Verdict::Unknown));
	ASSERT_FALSE(ShouldKeepLowIdSourceForIPv6Callback(true, true, false, EServerIPv6Verdict::Verified));
	ASSERT_FALSE(ShouldKeepLowIdSourceForIPv6Callback(true, false, true, EServerIPv6Verdict::Verified));
	ASSERT_FALSE(ShouldKeepLowIdSourceForIPv6Callback(false, true, true, EServerIPv6Verdict::Verified));
}
