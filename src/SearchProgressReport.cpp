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

#include "SearchProgressReport.h"

#include <ec/cpp/ECPacket.h>
#include <ec/cpp/ECCodes.h>
#include <common/Format.h>

#include <wx/intl.h> // _()

namespace
{

// Wire convention shared with amuleapi's SearchLifecycleStateToString and
// pinned against CSearchList::SearchLifecycleState by a static_assert in
// ExternalConn.cpp: 0 idle, 1 running, 2 finished.
wxString StateName(unsigned state)
{
	switch (state) {
	case 1:
		return _("running");
	case 2:
		return _("finished");
	default:
		return _("idle");
	}
}

wxString OneLine(unsigned id, const wxString &name, unsigned state, unsigned percent, unsigned results)
{
	// The query the search was started with, or the peer's nickname for a
	// browse. Older daemons do not send it; the line then reads as it did
	// before rather than showing an empty pair of quotes.
	if (name.IsEmpty()) {
		return CFormat(_("Search %u: %s, %u %% (%u results)\n")) % id % StateName(state) % percent %
		       results;
	}
	return CFormat(_("Search %u \"%s\": %s, %u %% (%u results)\n")) % id % name % StateName(state) %
	       percent % results;
}

wxString NameOf(const CECTag *tag)
{
	return tag ? tag->GetStringData() : wxString();
}

unsigned IntOr(const CECTag *tag, unsigned fallback)
{
	return tag ? static_cast<unsigned>(tag->GetInt()) : fallback;
}

} // namespace

wxString ecprogress::FormatSearchProgress(const CECPacket &response)
{
	if (response.GetTagByName(EC_TAG_SEARCH_EXPIRED)) {
		return _("Search expired or unknown ID. Start a new search.\n");
	}

	// Union shape: one child per search, its own value the search id and its
	// fields nested inside. Presence of EC_TAG_SEARCH_ID is NOT the test --
	// the single-search reply echoes one as a bare leaf, and an expired
	// verdict echoes the id it is about. Carrying the nested percent is what
	// makes a child an entry.
	wxString out;
	bool sawEntry = false;
	for (const CECTag &entry : response) {
		if (entry.GetTagName() != EC_TAG_SEARCH_ID) {
			continue;
		}
		const CECTag *percent = entry.GetTagByName(EC_TAG_SEARCH_LIFECYCLE_PERCENT);
		if (percent == nullptr) {
			continue;
		}
		sawEntry = true;
		out += OneLine(static_cast<unsigned>(entry.GetInt()),
			NameOf(entry.GetTagByName(EC_TAG_SEARCH_NAME)),
			IntOr(entry.GetTagByName(EC_TAG_SEARCH_LIFECYCLE_STATE), 0),
			static_cast<unsigned>(percent->GetInt()),
			IntOr(entry.GetTagByName(EC_TAG_SEARCH_RESULT_COUNT), 0));
	}
	if (sawEntry) {
		return out;
	}

	// Single-search reply carrying the lifecycle tags. Preferred over the
	// sentinel below: a finished search reports 100 % and says so, where the
	// sentinel only ever said 0 once the search stopped running.
	if (const CECTag *percent = response.GetTagByName(EC_TAG_SEARCH_LIFECYCLE_PERCENT)) {
		return OneLine(IntOr(response.GetTagByName(EC_TAG_SEARCH_ID), 0),
			NameOf(response.GetTagByName(EC_TAG_SEARCH_NAME)),
			IntOr(response.GetTagByName(EC_TAG_SEARCH_LIFECYCLE_STATE), 0),
			static_cast<unsigned>(percent->GetInt()),
			IntOr(response.GetTagByName(EC_TAG_SEARCH_RESULT_COUNT), 0));
	}

	// Oldest daemons: the overloaded sentinel alone. Values above 100 are its
	// done/failed markers rather than a percentage.
	if (const CECTag *status = response.GetTagByName(EC_TAG_SEARCH_STATUS)) {
		const unsigned progress = static_cast<unsigned>(status->GetInt());
		if (progress <= 100) {
			return CFormat(_("Search progress: %u %% \n")) % progress;
		}
		// Newline outside the msgid: the string is translated in 28
		// catalogs and changing it would orphan every one of them.
		return _("Search progress not available") + wxT("\n");
	}

	// A union reply with nothing in it: the daemon holds no searches. Distinct
	// from 0 %, which is a running search that has made no progress.
	return _("No search to report on.\n");
}
