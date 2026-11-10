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
#include "ServerIPv6Verdict.h"

using namespace muleunit;

DECLARE_SIMPLE(ServerIPv6Verdict)

TEST(ServerIPv6Verdict, AbsentStatusIsUnknown)
{
	ASSERT_TRUE(ParseServerIPv6Verdict(std::nullopt) == EServerIPv6Verdict::Unknown);
}

TEST(ServerIPv6Verdict, AllBitsIsVerified)
{
	ASSERT_TRUE(ParseServerIPv6Verdict(0x07) == EServerIPv6Verdict::Verified);
}

TEST(ServerIPv6Verdict, UnreachableProbeIsFailed)
{
	ASSERT_TRUE(ParseServerIPv6Verdict(0x05) == EServerIPv6Verdict::Failed);
}

TEST(ServerIPv6Verdict, HeldOnlyIsProbing)
{
	ASSERT_TRUE(ParseServerIPv6Verdict(0x01) == EServerIPv6Verdict::Probing);
}

TEST(ServerIPv6Verdict, UnprobedReachableIsTrusted)
{
	ASSERT_TRUE(ParseServerIPv6Verdict(0x03) == EServerIPv6Verdict::Trusted);
}

TEST(ServerIPv6Verdict, InvalidCombinationsAreUnknown)
{
	ASSERT_TRUE(ParseServerIPv6Verdict(0x00) == EServerIPv6Verdict::Unknown);
	ASSERT_TRUE(ParseServerIPv6Verdict(0x08) == EServerIPv6Verdict::Unknown);
}
