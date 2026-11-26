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

#include "KadFirewallCheckPolicy.h"

using namespace muleunit;

DECLARE_SIMPLE(KadFirewallCheckPolicy)

TEST(KadFirewallCheckPolicy, OurOwnTcpDialProvesThePort)
{
	ASSERT_TRUE(ProvesTcpPortReachable(EConnectionOpener::Dial, false));
}

TEST(KadFirewallCheckPolicy, AConnectionTheNodeOpenedProvesNothing)
{
	ASSERT_FALSE(ProvesTcpPortReachable(EConnectionOpener::Accepted, false));
	ASSERT_FALSE(ProvesTcpPortReachable(EConnectionOpener::AcceptedRendezvous, false));
	ASSERT_FALSE(ProvesTcpPortReachable(EConnectionOpener::AcceptedLugdunumRendezvous, false));
}

TEST(KadFirewallCheckPolicy, DialsThroughAPunchedMappingProveNothing)
{
	ASSERT_FALSE(ProvesTcpPortReachable(EConnectionOpener::HolePunchDial, false));
	ASSERT_FALSE(ProvesTcpPortReachable(EConnectionOpener::LugdunumCallbackDial, false));
}

TEST(KadFirewallCheckPolicy, CallbackAnswersAreRefused)
{
	ASSERT_FALSE(ProvesTcpPortReachable(EConnectionOpener::AnswerServerCallback, false));
	ASSERT_FALSE(ProvesTcpPortReachable(EConnectionOpener::AnswerKadCallback, false));
	ASSERT_FALSE(ProvesTcpPortReachable(EConnectionOpener::AnswerDirectCallback, false));
	ASSERT_FALSE(ProvesTcpPortReachable(EConnectionOpener::AnswerLugdunumCallback, false));
}

TEST(KadFirewallCheckPolicy, AStreamTransportDoesNotProveTheTcpPort)
{
	ASSERT_FALSE(ProvesTcpPortReachable(EConnectionOpener::Dial, true));
}
