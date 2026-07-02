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

#include "config.h" // Needed for VERSION_MJR / MIN / UPDATE (via ClientVersion.h)

#ifdef ENABLE_VERSION_CHECK

#include <common/ClientVersion.h> // VERSION_MJR / VERSION_MIN / VERSION_UPDATE
#include "OtherFunctions.h"       // make_full_ed2k_version
#include "Logger.h"               // AddDebugLogLineN / logGeneral

#include <wx/regex.h>
#include <wx/tokenzr.h>

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

	m_request = wxWebSession::GetDefault().CreateRequest(this, VERSION_CHECK_URL);
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
	// Extract the `tag_name` string — the same field the daemon-side check
	// reads. /releases/latest excludes pre-releases, so the tag names a
	// stable release. A regex on the one well-known field is robust against
	// whitespace and field-order changes without a full JSON parser.
	wxRegEx tagRe(wxT("\"tag_name\"[[:space:]]*:[[:space:]]*\"([^\"]+)\""));
	if (!tagRe.IsValid() || !tagRe.Matches(json)) {
		return Failed;
	}
	wxString tag = tagRe.GetMatch(json, 1);

	// Tolerate an optional leading v/V (aMule tags are bare semver, but be
	// defensive in case a future maintainer switches to vX.Y.Z).
	if (tag.StartsWith(wxT("v")) || tag.StartsWith(wxT("V"))) {
		tag = tag.Mid(1);
	}
	// Strip any pre-release / build-metadata suffix so the integer compare
	// sees only MAJOR.MINOR.UPDATE.
	size_t suffixPos = tag.find_first_of(wxT("-+"));
	if (suffixPos != wxString::npos) {
		tag = tag.Mid(0, suffixPos);
	}
	if (tag.IsEmpty()) {
		return Failed;
	}

	long fields[] = { 0, 0, 0 };
	wxStringTokenizer tkz(tag, wxT("."));
	for (int i = 0; i < 3 && tkz.HasMoreTokens(); ++i) {
		// Tags with fewer than three components (e.g. "3.1") are valid;
		// the missing fields stay 0.
		if (!tkz.GetNextToken().ToLong(&fields[i])) {
			return Failed;
		}
	}

	long curVer = make_full_ed2k_version(VERSION_MJR, VERSION_MIN, VERSION_UPDATE);
	long newVer = make_full_ed2k_version(fields[0], fields[1], fields[2]);
	if (curVer < newVer) {
		m_latest = wxString::Format(wxT("%ld.%ld.%ld"), fields[0], fields[1], fields[2]);
		return Outdated;
	}
	return UpToDate;
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
