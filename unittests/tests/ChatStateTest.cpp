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

// A failed chat connect must leave the chat able to dial again. eMule resets the state in
// CChatSelector::ConnectingResult() on success and on failure; aMule's port turned that into a GUI
// notification and dropped the reset, so after one failed connect every later message was queued
// behind "Already connecting?" and nothing was ever dialled again.

#include <muleunit/test.h>

#include "ChatState.h"

using namespace muleunit;

DECLARE_SIMPLE(ChatState)

// The client stays kept as a chat partner, and the next message redials.
TEST(ChatState, AFailedConnectLeavesTheChatReadyToRedial)
{
	ASSERT_EQUALS((unsigned)MS_CHATTING, (unsigned)ChatStateAfterFailedConnect(MS_CONNECTING));
}

// Only a pending connect is resolved; any other state is left as it was.
TEST(ChatState, AFailedConnectLeavesOtherStatesAlone)
{
	ASSERT_EQUALS((unsigned)MS_NONE, (unsigned)ChatStateAfterFailedConnect(MS_NONE));
	ASSERT_EQUALS((unsigned)MS_CHATTING, (unsigned)ChatStateAfterFailedConnect(MS_CHATTING));
	ASSERT_EQUALS(
		(unsigned)MS_UNABLETOCONNECT, (unsigned)ChatStateAfterFailedConnect(MS_UNABLETOCONNECT));
}

// File_checked_for_headers
