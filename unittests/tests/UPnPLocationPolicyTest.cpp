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

#include "UPnPLocationPolicy.h"

using namespace muleunit;

DECLARE_SIMPLE(UPnPLocationPolicy)

TEST(UPnPLocationPolicy, AnIPv4LocationIsUsed)
{
	ASSERT_FALSE(UPnP::IsIPv6Location("http://192.0.2.1:5000/rootDesc.xml"));
}

TEST(UPnPLocationPolicy, AnIPv6LocationIsIgnored)
{
	ASSERT_TRUE(UPnP::IsIPv6Location("http://[2001:db8::1]:5000/rootDesc.xml"));
	ASSERT_TRUE(UPnP::IsIPv6Location("http://[fe80::1%25eth0]:49000/igddesc.xml"));
	ASSERT_TRUE(UPnP::IsIPv6Location("http://[::1]/desc.xml"));
}

TEST(UPnPLocationPolicy, AHostNameIsUsed)
{
	ASSERT_FALSE(UPnP::IsIPv6Location("http://gateway.example:49000/igddesc.xml"));
}

TEST(UPnPLocationPolicy, OnlyTheHostCounts)
{
	ASSERT_FALSE(UPnP::IsIPv6Location("http://192.0.2.1:5000/[::1]"));
	ASSERT_FALSE(UPnP::IsIPv6Location("http://192.0.2.1:5000/?next=http://[::1]/"));
}

TEST(UPnPLocationPolicy, AMalformedLocationIsLeftToTheFetch)
{
	ASSERT_FALSE(UPnP::IsIPv6Location(""));
	ASSERT_FALSE(UPnP::IsIPv6Location("http://"));
	ASSERT_FALSE(UPnP::IsIPv6Location("[::1]:5000/rootDesc.xml"));
}
