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

#include <QuicStreamAcceptor.h>
#include <libs/common/Format.h>

using namespace muleunit;

DECLARE_SIMPLE(QuicAdmission)

namespace
{
const char *Name(EQuicAdmission value)
{
	switch (value) {
	case EQuicAdmission::Admit:
		return "Admit";
	case EQuicAdmission::ShuttingDown:
		return "ShuttingDown";
	case EQuicAdmission::TooManySockets:
		return "TooManySockets";
	case EQuicAdmission::NoAddress:
		return "NoAddress";
	case EQuicAdmission::Filtered:
		return "Filtered";
	case EQuicAdmission::Banned:
		return "Banned";
	}
	return "?";
}
} // namespace

TEST(QuicAdmission, DecisionTable)
{
	const struct
	{
		const char *label;
		bool running;
		bool tooManySockets;
		bool hasAddress;
		bool filtered;
		bool banned;
		EQuicAdmission expected;
	} cases[] = {
		{ "healthy peer", true, false, true, false, false, EQuicAdmission::Admit },
		{ "shutting down", false, false, true, false, false, EQuicAdmission::ShuttingDown },
		{ "shutdown outranks every other reason",
			false,
			true,
			false,
			true,
			true,
			EQuicAdmission::ShuttingDown },
		// The caller passes the limit as it stands: no exception while connecting to a server.
		{ "at the connection limit", true, true, true, false, false, EQuicAdmission::TooManySockets },
		{ "the limit outranks a missing address",
			true,
			true,
			false,
			false,
			false,
			EQuicAdmission::TooManySockets },
		{ "the limit outranks a filtered address",
			true,
			true,
			true,
			true,
			false,
			EQuicAdmission::TooManySockets },
		{ "no address", true, false, false, false, false, EQuicAdmission::NoAddress },
		{ "filtered", true, false, true, true, false, EQuicAdmission::Filtered },
		{ "banned", true, false, true, false, true, EQuicAdmission::Banned },
		// The reason reaches the log and the statistics, so the order is observable.
		{ "filtered outranks banned", true, false, true, true, true, EQuicAdmission::Filtered },
	};
	for (const auto &row : cases) {
		const EQuicAdmission actual = DecideQuicAdmission(
			row.running, row.tooManySockets, row.hasAddress, row.filtered, row.banned);
		CFormat format("%s: expected %s, got %s");
		const wxString message = format % row.label % Name(row.expected) % Name(actual);
		ASSERT_TRUE_M(actual == row.expected, message);
	}
}

TEST(QuicAdmission, OnlyOneOutcomeAdmits)
{
	for (unsigned mask = 0; mask < 32; ++mask) {
		const bool running = (mask & 1) != 0;
		const bool tooManySockets = (mask & 2) != 0;
		const bool hasAddress = (mask & 4) != 0;
		const bool filtered = (mask & 8) != 0;
		const bool banned = (mask & 16) != 0;
		const bool admits =
			DecideQuicAdmission(running, tooManySockets, hasAddress, filtered, banned) ==
			EQuicAdmission::Admit;
		ASSERT_EQUALS(running && !tooManySockets && hasAddress && !filtered && !banned, admits);
	}
}
