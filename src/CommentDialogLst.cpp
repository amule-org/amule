//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2002-2011 Merkur ( devs@emule-project.net / http://www.emule-project.net )
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

#include "CommentDialogLst.h" // Interface declarations
#include "muuli_wdr.h"        // Needed for commentLstDlg
#include "PartFile.h"         // Needed for CPartFile
#include <common/Format.h>    // Needed for CFormat
#include "MuleListCtrl.h"     // Needed for CMuleListCtrl
#include "Preferences.h"
#include "amule.h" // Needed for theApp

#include <set>

// Timer id for the Kad-notes auto-refresh (routed to this dialog only).
static const int ID_KADREFRESH_TIMER = wxID_HIGHEST + 501;

wxBEGIN_EVENT_TABLE(CCommentDialogLst, wxDialog)
	EVT_BUTTON(IDCOK, CCommentDialogLst::OnBnClickedApply)
	EVT_BUTTON(IDCREF, CCommentDialogLst::OnBnClickedRefresh)
	EVT_BUTTON(IDC_CMSEARCHKAD, CCommentDialogLst::OnBnClickedSearchKad)
	EVT_TIMER(ID_KADREFRESH_TIMER, CCommentDialogLst::OnKadRefreshTimer)
wxEND_EVENT_TABLE()

namespace
{
// Registry of open CCommentDialogLst instances. See CCommentDialog.cpp
// for the rationale — the broadcast handler in GuiEvents.cpp iterates
// this on every CKnownFile destruction and dismisses any dialog whose
// m_file has just been freed (UAF prevention; #755 / #748 family).
std::set<CCommentDialogLst *> &OpenInstances()
{
	static std::set<CCommentDialogLst *> instances;
	return instances;
}
} // namespace

/*
 * Constructor
 */
CCommentDialogLst::CCommentDialogLst(wxWindow *parent, CPartFile *file)
: wxDialog(parent,
	  -1,
	  wxString(_("File Comments")),
	  wxDefaultPosition,
	  wxDefaultSize,
	  wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
, m_file(file)
, m_kadRefreshTimer(this, ID_KADREFRESH_TIMER)
, m_kadRefreshTicks(0)
{
	wxSizer *content = commentLstDlg(this, true);
	content->Show(this, true);

	m_list = CastChild(IDC_LST, CMuleListCtrl);
	m_list->InsertColumn(0, _("Username"), wxLIST_FORMAT_LEFT, 130);
	m_list->InsertColumn(1, _("File Name"), wxLIST_FORMAT_LEFT, 130);
	m_list->InsertColumn(2, _("Rating"), wxLIST_FORMAT_LEFT, 80);
	m_list->InsertColumn(3, _("Comment"), wxLIST_FORMAT_LEFT, 340);
	m_list->SetSortFunc(SortProc);

	UpdateList();
	OpenInstances().insert(this);
}

CCommentDialogLst::~CCommentDialogLst()
{
	m_kadRefreshTimer.Stop();
	OpenInstances().erase(this);
	ClearList();
}

void CCommentDialogLst::DropReferencesTo(const CKnownFile *file)
{
	for (CCommentDialogLst *d : OpenInstances()) {
		// m_file is a CPartFile* — compare against the up-cast.
		if (static_cast<const CKnownFile *>(d->m_file) == file) {
			d->m_file = NULL;
			d->EndModal(0);
		}
	}
}

void CCommentDialogLst::OnBnClickedApply(wxCommandEvent &WXUNUSED(evt))
{
	EndModal(0);
}

void CCommentDialogLst::OnBnClickedRefresh(wxCommandEvent &WXUNUSED(evt))
{
	UpdateList();
}

void CCommentDialogLst::OnBnClickedSearchKad(wxCommandEvent &WXUNUSED(evt))
{
	if (!m_file) {
		return;
	}

#ifdef CLIENT_GUI
	// amulegui has no local Kad; ask the daemon to run the lookup. Retrieved notes
	// arrive through the normal partfile-comments update. Mark the search running
	// optimistically — the daemon's real state overwrites this on the next update.
	theApp->sharedfiles->SearchKadNotes(m_file);
	m_file->SetKadCommentSearchRunning(true);
#else
	if (!m_file->RequestKadNoteSearch()) {
		// Kad down, or a search (often the file's own source search) is already
		// using this hash. The daemon log (logKadSearch) records the exact reason;
		// reuse existing strings here to avoid new catalog entries.
		FindWindow(IDC_CMSTATUS)
			->SetLabel(theApp->IsConnectedKad()
					   ? _("Could not start a Kad search")
					   : _("Kad search can't be done if Kad is not running"));
		FindWindow(IDC_CMSTATUS)->GetParent()->Layout();
		return;
	}
#endif

	// The lookup is asynchronous (up to ~45s). Disable the button, show progress,
	// and poll: the timer refreshes the list as notes arrive and stops when done.
	FindWindow(IDC_CMSEARCHKAD)->Disable();
	FindWindow(IDC_CMSTATUS)->SetLabel(_("Searching Kad for comments..."));
	FindWindow(IDC_CMSTATUS)->GetParent()->Layout();
	m_kadRefreshTicks = 0;
	m_kadRefreshTimer.Start(2000);
}

void CCommentDialogLst::OnKadRefreshTimer(wxTimerEvent &WXUNUSED(evt))
{
	// A Kad notes lookup lives at most SEARCHNOTES_LIFETIME (45s); cap at 60s so the
	// timer can never hang even if the "running" flag is never observed to clear.
	const bool timedOut = ++m_kadRefreshTicks > 30;

	if (!m_file || timedOut || !m_file->IsKadCommentSearchRunning()) {
		// Search finished (or the file went away): final refresh + restore the UI.
		m_kadRefreshTimer.Stop();
		if (FindWindow(IDC_CMSEARCHKAD)) {
			FindWindow(IDC_CMSEARCHKAD)->Enable();
		}
		if (m_file) {
#ifdef CLIENT_GUI
			// If we bailed on the safety timeout, clear the optimistic flag so a
			// later search can start (the daemon is authoritative otherwise).
			if (timedOut) {
				m_file->SetKadCommentSearchRunning(false);
			}
#endif
			UpdateList();
		}
		return;
	}

	// Still running: show whatever notes have arrived so far, keep the label.
	UpdateList();
	FindWindow(IDC_CMSTATUS)->SetLabel(_("Searching Kad for comments..."));
	FindWindow(IDC_CMSTATUS)->GetParent()->Layout();
}

void CCommentDialogLst::UpdateList()
{
	int count = 0;
	ClearList();

	FileRatingList list;
	m_file->GetRatingAndComments(list);
	for (FileRatingList::const_iterator it = list.begin(); it != list.end(); ++it) {
		if (!thePrefs::IsCommentFiltered(it->Comment)) {
			m_list->InsertItem(count, it->UserName);
			m_list->SetItem(count, 1, it->FileName);
			m_list->SetItem(
				count, 2, (it->Rating != -1) ? GetRateString(it->Rating) : wxString("on"));
			m_list->SetItem(count, 3, it->Comment);
			m_list->SetItemPtrData(count, reinterpret_cast<wxUIntPtr>(new SFileRating(*it)));
			++count;
		}
	}

	wxString info;
	if (count == 0) {
		info = _("No comments");
	} else {
		info = CFormat(wxPLURAL("%u comment", "%u comments", count)) % count;
	}

	FindWindow(IDC_CMSTATUS)->SetLabel(info);
	FindWindow(IDC_CMSTATUS)->GetParent()->Layout();

	m_file->UpdateFileRatingCommentAvail();
}

void CCommentDialogLst::ClearList()
{
	size_t count = m_list->GetItemCount();
	for (size_t i = 0; i < count; ++i) {
		delete reinterpret_cast<SFileRating *>(m_list->GetItemData(i));
	}

	m_list->DeleteAllItems();
}

int CCommentDialogLst::SortProc(wxUIntPtr item1, wxUIntPtr item2, wxIntPtr sortData)
{
	SFileRating *file1 = reinterpret_cast<SFileRating *>(item1);
	SFileRating *file2 = reinterpret_cast<SFileRating *>(item2);

	int mod = (sortData & CMuleListCtrl::SORT_DES) ? -1 : 1;

	switch (sortData & CMuleListCtrl::COLUMN_MASK) {
	case 0:
		return mod * file1->UserName.Cmp(file2->UserName);
	case 1:
		return mod * file1->FileName.Cmp(file2->FileName);
	case 2:
		return mod * (file1->Rating - file2->Rating);
	case 3:
		return mod * file1->Comment.Cmp(file2->Comment);
	default:
		return 0;
	}
}
// File_checked_for_headers
