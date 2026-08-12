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

#ifndef CLIENTNAMECELL_H
#define CLIENTNAMECELL_H

#include <wx/gdicmn.h> // Needed for wxRect
#include <wx/string.h>

#include "Types.h"

class CUpDownClient;
class wxDC;

/**
 * Everything the User Name cell draws, as values.
 *
 * A snapshot rather than a client pointer because the two lists that draw this
 * cell reach their peers differently: the per-file lists hold an owning
 * CClientRef and could be asked at paint time, while the global clients list
 * copies each peer's values once per sweep precisely so that nothing it paints
 * can be freed underneath it. A value type is the only input both can produce,
 * and it keeps the drawing free of any lifetime question.
 */
struct ClientNameCell
{
	wxString name;
	//! Two-letter code, or empty to draw no flag. Already through
	//! GetDisplayCountryCode(), so an empty string means "draw nothing"
	//! whether that is because the feature is off or the country is unknown.
	wxString countryCode;
	uint8 downloadState = 0; //!< DS_*
	uint8 clientSoft = 0;    //!< SO_*
	uint8 obfuscation = 0;   //!< OBST_*
	//! An A4AF source is drawn grey whatever its download state says.
	bool a4af = false;
	/**
	 * Draw the download-state badge.
	 *
	 * False for a row describing a peer we are not talking to -- the history
	 * list -- where there is no live state and a badge could only invent one.
	 */
	bool showState = true;
	//! False when the software is simply not recorded, so it draws as unknown
	//! rather than as whatever SO_* value zero happens to mean (eMule).
	bool knownSoftware = true;
	bool remoteQueueFull = false;
	bool isFriend = false;
	bool identified = false;
	bool badGuy = false;
	bool extProtocol = true;
	//! Score ratio above 1, i.e. we owe this peer.
	bool highCredits = false;
};

//! Snapshot a live peer. Safe to call only while `client` is known alive.
ClientNameCell MakeClientNameCell(const CUpDownClient *client, bool a4af = false);

/**
 * Draw the badge cluster, the optional country flag and the name.
 *
 * Several badges deliberately stack at the same x: the download-state smiley
 * advances, then software/credentials/encryption all overprint one slot, which
 * is what the pre-wxDataView DrawClientItem() did and what the column widths
 * are still sized for.
 */
void DrawClientNameCell(const ClientNameCell &cell, const wxRect &rect, wxDC *dc);

#endif // CLIENTNAMECELL_H
// File_checked_for_headers
