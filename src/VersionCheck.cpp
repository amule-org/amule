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

#include "VersionCheck.h"

#include "config.h" // Needed for ENABLE_VERSION_CHECK

#ifdef ENABLE_VERSION_CHECK

#include "OtherFunctions.h" // CompareLatestReleaseVersion
#include "Logger.h"         // AddDebugLogLineN / logGeneral
#include "HTTPDownload.h"   // CreateAmuleWebRequest (shared curl session + interface bind)

wxDEFINE_EVENT(wxEVT_VERSION_CHECK_DONE, wxCommandEvent);

// GitHub Releases "latest" endpoint — returns the most recent
// non-prerelease, non-draft release as JSON. Same source the daemon-side
// CamuleApp::CheckNewVersion uses.
static const wxString VERSION_CHECK_URL = wxT("https://api.github.com/repos/amule-org/amule/releases/latest");

CVersionCheck::CVersionCheck()
: m_notify(NULL)
, m_notifyId(wxID_ANY)
, m_status(Failed)
{
}

CVersionCheck::~CVersionCheck()
{
	if (m_request.IsOk() && m_request.GetState() == wxWebRequest::State_Active) {
		m_request.Cancel();
	}
}

void CVersionCheck::Start(wxEvtHandler *notify, int notifyId)
{
	if (m_status == Checking) {
		return;
	}
	m_notify = notify;
	m_notifyId = notifyId;
	m_status = Checking;
	m_latest.Clear();

	// Shared aMule HTTP path: curl-backed session, proxy, and egress bound to
	// the configured network interface when one is set (so the check doesn't
	// leak past a bound interface).
	m_request = CreateAmuleWebRequest(this, VERSION_CHECK_URL);
	if (!m_request.IsOk()) {
		Finish(Failed);
		return;
	}
	// The body is a few KB of JSON — keep it in memory, no temp file.
	m_request.SetStorage(wxWebRequest::Storage_Memory);
	// GitHub's API rejects requests without a User-Agent header.
	m_request.SetHeader(wxT("User-Agent"), wxT("aMule"));
	m_request.SetHeader(wxT("Accept"), wxT("application/vnd.github+json"));
	Bind(wxEVT_WEBREQUEST_STATE, &CVersionCheck::OnRequestState, this);
	m_request.Start();
}

void CVersionCheck::OnRequestState(wxWebRequestEvent &evt)
{
	switch (evt.GetState()) {
	case wxWebRequest::State_Completed:
		Finish(Evaluate(evt.GetResponse().AsString()));
		break;
	case wxWebRequest::State_Failed:
	case wxWebRequest::State_Unauthorized:
		AddDebugLogLineN(logGeneral, wxT("Version check failed: ") + evt.GetErrorDescription());
		Finish(Failed);
		break;
	case wxWebRequest::State_Cancelled:
		Finish(Failed);
		break;
	default:
		// State_Idle / State_Active — request still in progress.
		break;
	}
}

CVersionCheck::Status CVersionCheck::Evaluate(const wxString &json)
{
	// Shared parse + compare (see OtherFunctions.cpp:CompareLatestReleaseVersion).
	// Same logic the daemon check and amuleapi use, so there is a single
	// source of truth for how a GitHub release tag is interpreted.
	const CVersionCompareResult r = CompareLatestReleaseVersion(json);
	switch (r.state) {
	case CVersionCompareResult::Outdated:
		m_latest = r.latest;
		return Outdated;
	case CVersionCompareResult::UpToDate:
		return UpToDate;
	case CVersionCompareResult::ParseError:
	default:
		return Failed;
	}
}

void CVersionCheck::Finish(Status status)
{
	m_status = status;
	if (m_notify) {
		wxCommandEvent evt(wxEVT_VERSION_CHECK_DONE, m_notifyId);
		evt.SetInt(status);
		m_notify->AddPendingEvent(evt);
	}
}

#endif // ENABLE_VERSION_CHECK
