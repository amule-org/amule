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

#include <UtpStreamAcceptor.h>
#include <libs/common/Format.h>

using namespace muleunit;

DECLARE_SIMPLE(UtpAdmission)

namespace
{
constexpr uint32_t kPeer = 0x0100007F;

const char *Name(EUtpAdmission value)
{
	switch (value) {
	case EUtpAdmission::Admit:
		return "Admit";
	case EUtpAdmission::ShuttingDown:
		return "ShuttingDown";
	case EUtpAdmission::TooManySockets:
		return "TooManySockets";
	case EUtpAdmission::TooManyHalfOpen:
		return "TooManyHalfOpen";
	case EUtpAdmission::NoAddress:
		return "NoAddress";
	case EUtpAdmission::Filtered:
		return "Filtered";
	case EUtpAdmission::Banned:
		return "Banned";
	}
	return "?";
}
} // namespace

TEST(UtpAdmission, DecisionTable)
{
	const struct
	{
		const char *label;
		bool running;
		bool tooManySockets;
		std::size_t halfOpen;
		uint32_t ip;
		bool filtered;
		bool banned;
		EUtpAdmission expected;
	} cases[] = {
		{ "healthy peer", true, false, 0, kPeer, false, false, EUtpAdmission::Admit },
		{ "shutting down", false, false, 0, kPeer, false, false, EUtpAdmission::ShuttingDown },
		{ "shutdown outranks every other reason",
			false,
			true,
			kMaxHalfOpenUtpStreams,
			0,
			true,
			true,
			EUtpAdmission::ShuttingDown },
		{ "at the connection limit",
			true,
			true,
			0,
			kPeer,
			false,
			false,
			EUtpAdmission::TooManySockets },
		// No exception while connecting to a server: that is the TCP listener's, for the server's
		// TCP callback test.
		{ "half-open streams below the budget",
			true,
			false,
			kMaxHalfOpenUtpStreams - 1,
			kPeer,
			false,
			false,
			EUtpAdmission::Admit },
		{ "half-open budget spent",
			true,
			false,
			kMaxHalfOpenUtpStreams,
			kPeer,
			false,
			false,
			EUtpAdmission::TooManyHalfOpen },
		{ "the connection limit outranks the half-open budget",
			true,
			true,
			kMaxHalfOpenUtpStreams,
			kPeer,
			false,
			false,
			EUtpAdmission::TooManySockets },
		{ "no address", true, false, 0, 0, false, false, EUtpAdmission::NoAddress },
		{ "filtered", true, false, 0, kPeer, true, false, EUtpAdmission::Filtered },
		{ "banned", true, false, 0, kPeer, false, true, EUtpAdmission::Banned },
		// Ordering is observable through the reason, and the reason is what
		// reaches the log and the statistics.
		{ "filtered outranks banned", true, false, 0, kPeer, true, true, EUtpAdmission::Filtered },
		{ "the limit outranks a filtered address",
			true,
			true,
			0,
			kPeer,
			true,
			false,
			EUtpAdmission::TooManySockets },
	};
	for (const auto &row : cases) {
		const EUtpAdmission actual = DecideUtpAdmission(
			row.running, row.tooManySockets, row.halfOpen, row.ip, row.filtered, row.banned);
		CFormat format("%s: expected %s, got %s");
		const wxString message = format % row.label % Name(row.expected) % Name(actual);
		ASSERT_TRUE_M(actual == row.expected, message);
	}
}

TEST(UtpAdmission, OnlyOneOutcomeAdmits)
{
	// A stream is taken for one reason and refused for several. Anything that
	// admits by falling through a new case would show up here.
	ASSERT_TRUE(DecideUtpAdmission(true, false, 0, kPeer, false, false) == EUtpAdmission::Admit);
	ASSERT_FALSE(DecideUtpAdmission(false, false, 0, kPeer, false, false) == EUtpAdmission::Admit);
	ASSERT_FALSE(DecideUtpAdmission(true, true, 0, kPeer, false, false) == EUtpAdmission::Admit);
	ASSERT_FALSE(DecideUtpAdmission(true, false, 0, 0, false, false) == EUtpAdmission::Admit);
	ASSERT_FALSE(DecideUtpAdmission(true, false, 0, kPeer, true, false) == EUtpAdmission::Admit);
	ASSERT_FALSE(DecideUtpAdmission(true, false, 0, kPeer, false, true) == EUtpAdmission::Admit);
}

// File_checked_for_headers
