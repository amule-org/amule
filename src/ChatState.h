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

#ifndef CHATSTATE_H
#define CHATSTATE_H

#include "Constants.h" // Needed for MS_*

#include <cstdint>

/**
 * The chat state once a connect made for a chat has failed.
 *
 * A pending connect becomes MS_CHATTING, as in eMule's CChatSelector::ConnectingResult(): the client
 * stays kept as a chat partner, and the next message dials again instead of being queued behind a
 * connect that will never complete.
 */
inline std::uint8_t ChatStateAfterFailedConnect(std::uint8_t state)
{
	return state == MS_CONNECTING ? static_cast<std::uint8_t>(MS_CHATTING) : state;
}

#endif // CHATSTATE_H
