//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2002-2011 Merkur ( devs@emule-project.net / http://www.emule-project.net )
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

#include "ClientVersionString.h"

#include <muleunit/test.h>
#include <protocol/ed2k/ClientSoftware.h>

using namespace muleunit;

DECLARE_SIMPLE(ClientVersionString)

// The reported case: eMule spells its update as a letter, and the history rows
// rendered it as a digit (amule-org/amule#1127).
TEST(ClientVersionString, EMuleLettersItsUpdateComponent)
{
	ASSERT_EQUALS(wxT("v0.70a"), FormatClientVersion(SO_EMULE, 0, 70, 0));
	ASSERT_EQUALS(wxT("v0.70b"), FormatClientVersion(SO_EMULE, 0, 70, 1));
	ASSERT_EQUALS(wxT("v0.49d"), FormatClientVersion(SO_EMULE, 0, 49, 3));
}

// An unknown software is rendered as eMule, which is what the handshake's
// default branch did.
TEST(ClientVersionString, AnUnknownSoftwareFollowsEMule)
{
	ASSERT_EQUALS(wxT("v0.60a"), FormatClientVersion(0xfe, 0, 60, 0));
}

TEST(ClientVersionString, TheMuleFamilyUsesThreeNumbers)
{
	ASSERT_EQUALS(wxT("v2.3.3"), FormatClientVersion(SO_AMULE, 2, 3, 3));
	ASSERT_EQUALS(wxT("v1.9.1"), FormatClientVersion(SO_LXMULE, 1, 9, 1));
	ASSERT_EQUALS(wxT("v3.1.6"), FormatClientVersion(SO_MLDONKEY, 3, 1, 6));
	ASSERT_EQUALS(wxT("v0.0.0"), FormatClientVersion(SO_HYDRANODE, 0, 0, 0));
}

// The hybrid drops a zero update rather than printing it.
TEST(ClientVersionString, TheHybridOmitsAZeroUpdate)
{
	ASSERT_EQUALS(wxT("v1.50"), FormatClientVersion(SO_EDONKEYHYBRID, 1, 50, 0));
	ASSERT_EQUALS(wxT("v1.50.2"), FormatClientVersion(SO_EDONKEYHYBRID, 1, 50, 2));
}

// eMule Plus omits zero components, and letters from 'a' meaning 1 -- one off
// from eMule, where 'a' means 0.
TEST(ClientVersionString, EMulePlusOmitsZeroComponents)
{
	ASSERT_EQUALS(wxT("v1"), FormatClientVersion(SO_EMULEPLUS, 1, 0, 0));
	ASSERT_EQUALS(wxT("v1.2"), FormatClientVersion(SO_EMULEPLUS, 1, 2, 0));
	ASSERT_EQUALS(wxT("v1.2a"), FormatClientVersion(SO_EMULEPLUS, 1, 2, 1));
	ASSERT_EQUALS(wxT("v1.2b"), FormatClientVersion(SO_EMULEPLUS, 1, 2, 2));
}

TEST(ClientVersionString, LPhantCountsItsMajorOneHigher)
{
	// major 1 on the wire is lPhant 0.x, and the minor is padded to two.
	ASSERT_EQUALS(wxT(" v0.05a"), FormatClientVersion(SO_LPHANT, 1, 5, 0));
	ASSERT_EQUALS(wxT(" v0.51b"), FormatClientVersion(SO_LPHANT, 1, 51, 1));
}

// MAKE_CLIENT_VERSION is a decimal composite, not a bitfield. The history rows
// store that form, so the packed helper has to decompose it the same way.
TEST(ClientVersionString, ThePackedFormDecomposesDecimally)
{
	// 0 * 100000 + 70 * 1000 + 1 * 100 -- eMule 0.70b, the reported row.
	ASSERT_EQUALS(wxT("v0.70b"), FormatPackedClientVersion(SO_EMULE, 70100));
	ASSERT_EQUALS(wxT("v0.70a"), FormatPackedClientVersion(SO_EMULE, 70000));
	// 2 * 100000 + 3 * 1000 + 3 * 100 -- aMule 2.3.3.
	ASSERT_EQUALS(wxT("v2.3.3"), FormatPackedClientVersion(SO_AMULE, 203300));
}

// The whole point: one peer must not read differently depending on which path
// rendered it. The live path decodes the wire into components; the history
// path decodes the stored composite. Both must land on the same string.
TEST(ClientVersionString, BothPathsAgreeForTheSamePeer)
{
	const uint32 major = 0, minor = 70, update = 1;
	const uint32 packed = major * 100000 + minor * 1000 + update * 100;
	ASSERT_EQUALS(FormatClientVersion(SO_EMULE, major, minor, update),
		FormatPackedClientVersion(SO_EMULE, packed));
}
