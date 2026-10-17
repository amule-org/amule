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

// How we publish ourselves as a Kad source. A node firewalled on IPv4 with neither a direct UDP
// callback nor a buddy published nothing, although peers that hold an IPv6 route could reach it at
// the IPv6 address it advertises: aMule since #1817 and eMuleAI add a firewalled source by its "ip6"
// tag alone.

#include <muleunit/test.h>

#include "KadPublishPolicy.h"

using namespace muleunit;

DECLARE_SIMPLE(KadPublishPolicy)

namespace
{
SKadPublishFacts Firewalled()
{
	SKadPublishFacts facts;
	facts.firewalled = true;
	return facts;
}
} // namespace

TEST(KadPublishPolicy, AnOpenNodePublishesAsHighID)
{
	SKadPublishFacts open;
	open.advertisesIPv6 = true;
	ASSERT_TRUE(DecideKadSourcePublish(open) == EKadSourcePublish::HighID);
}

// The IPv4 callback routes keep their precedence: they reach peers with no IPv6 route as well, and
// the "ip6" tag rides along with them anyway.
TEST(KadPublishPolicy, AnIPv4CallbackRouteIsPreferredOverIPv6Alone)
{
	SKadPublishFacts direct = Firewalled();
	direct.directCallback = true;
	direct.hasBuddy = true;
	direct.advertisesIPv6 = true;
	ASSERT_TRUE(DecideKadSourcePublish(direct) == EKadSourcePublish::DirectCallback);

	SKadPublishFacts buddy = Firewalled();
	buddy.hasBuddy = true;
	buddy.advertisesIPv6 = true;
	ASSERT_TRUE(DecideKadSourcePublish(buddy) == EKadSourcePublish::Buddy);
}

// The case this adds: no IPv4 route back to us, but an IPv6 address we advertise.
TEST(KadPublishPolicy, AFirewalledNodeWithoutCallbackPublishesOnItsIPv6)
{
	SKadPublishFacts facts = Firewalled();
	facts.advertisesIPv6 = true;
	ASSERT_TRUE(DecideKadSourcePublish(facts) == EKadSourcePublish::IPv6Only);
}

// Without any route back there is nothing to publish, as before.
TEST(KadPublishPolicy, AFirewalledNodeWithNoRouteBackPublishesNothing)
{
	const SKadPublishFacts facts = Firewalled();
	ASSERT_TRUE(DecideKadSourcePublish(facts) == EKadSourcePublish::None);
}

// File_checked_for_headers
