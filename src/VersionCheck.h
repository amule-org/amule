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

#ifndef VERSIONCHECK_H
#define VERSIONCHECK_H

#include "config.h" // for ENABLE_VERSION_CHECK

#ifdef ENABLE_VERSION_CHECK

#include <wx/event.h>
#include <wx/string.h>
#include <wx/webrequest.h>

// Posted (as a wxCommandEvent) to the handler passed to CVersionCheck::Start()
// when a check finishes. GetInt() carries the resulting CVersionCheck::Status.
wxDECLARE_EVENT(wxEVT_VERSION_CHECK_DONE, wxCommandEvent);

// Fetches the latest published aMule release from GitHub and compares it
// against the running client version (VERSION_MJR / MIN / UPDATE).
//
// Shared by the monolithic GUI and amulegui: both are GUI clients built from
// the same version macros, so neither needs the daemon core nor an EC
// round-trip to answer "is a newer aMule available?".
//
// Asynchronous: Start() kicks off a wxWebRequest and returns immediately; the
// result arrives later as a wxEVT_VERSION_CHECK_DONE event on the caller's
// handler. One instance runs at most one check at a time.
class CVersionCheck : public wxEvtHandler
{
public:
	enum Status
	{
		Checking, // a request is currently in flight
		UpToDate, // running version is the latest release (or newer)
		Outdated, // a newer release exists — see LatestVersion()
		Failed    // network or parse error
	};

	CVersionCheck();
	~CVersionCheck() override;

	// Start an asynchronous check. On completion a wxEVT_VERSION_CHECK_DONE
	// command event with id == notifyId is posted to notify. A Start() call
	// while a check is already in flight is ignored.
	void Start(wxEvtHandler *notify, int notifyId);

	Status GetStatus() const { return m_status; }

	// Latest release version as "X.Y.Z"; populated when the status is
	// Outdated, empty otherwise.
	const wxString &LatestVersion() const { return m_latest; }

private:
	void OnRequestState(wxWebRequestEvent &evt);
	void Finish(Status status);

	// Parse the /releases/latest JSON, extract tag_name, and compare it
	// against the running version. Sets m_latest on Outdated.
	Status Evaluate(const wxString &json);

	wxWebRequest m_request;
	wxEvtHandler *m_notify;
	int m_notifyId;
	Status m_status;
	wxString m_latest;
};

#endif // ENABLE_VERSION_CHECK

#endif // VERSIONCHECK_H
