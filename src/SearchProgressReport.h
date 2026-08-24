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
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA
//

#ifndef SEARCHPROGRESSREPORT_H
#define SEARCHPROGRESSREPORT_H

#include <wx/string.h>

class CECPacket;

namespace ecprogress
{

/**
 * Render an `EC_OP_SEARCH_PROGRESS` reply as amulecmd's `progress` output.
 *
 * The reply comes in three shapes and the client does not get to choose which:
 * a daemon that echoed `EC_TAG_CAN_SEARCH_PROGRESS_UNION` always answers with
 * the union, one entry per search, naming ids only narrows which entries come
 * back. Older daemons answer about a single search, with the lifecycle tags at
 * the top level or, older still, only the overloaded `EC_TAG_SEARCH_STATUS`
 * sentinel.
 *
 * Reading one shape as another is what made `progress` report 0 %
 * unconditionally: the union nests `EC_TAG_SEARCH_STATUS` inside each entry,
 * so a top-level read found nothing and rendered the miss as a number.
 *
 * Kept apart from the command loop, and free of theApp, so all three shapes
 * are reachable from a test.
 */
wxString FormatSearchProgress(const CECPacket &response);

} // namespace ecprogress

#endif // SEARCHPROGRESSREPORT_H
