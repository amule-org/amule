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

#include <wx/app.h>
#include <wx/clipbrd.h>  // Needed for wxTheClipboard (search-name Paste enable check)
#include <wx/combobox.h> // Needed for the IDC_SEARCHNAME history dropdown
#include <wx/config.h>   // Needed to persist the default search type
#include <wx/dataobj.h>  // Needed for wxTextDataObject (clipboard content check)
#include <wx/menu.h>     // Needed for the search-history context menu

#include <wx/gauge.h> // Do_not_auto_remove (win32)

#include <algorithm> // Needed for std::min

#include <tags/FileTags.h>

#include "SearchDlg.h"      // Interface declarations.
#include "SearchHistory.h"  // Needed for ApplySearchHistoryEntry
#include "SearchListCtrl.h" // Needed for CSearchListCtrl
#include "muuli_wdr.h"      // Needed for IDC_STARTS
#include "amuleDlg.h"       // Needed for CamuleDlg
#include "MuleNotebook.h"
#include "GetTickCount.h"
#include "Preferences.h"
#include "amule.h"        // Needed for theApp
#include "SearchList.h"   // Needed for CSearchList
#include "updownclient.h" // Needed for EBrowseStatus (browse tab lifecycle)
#include <common/Format.h>
#include <common/TextFile.h> // Needed for CTextFile (searchhistory.dat)
#include "Logger.h"

#define ID_SEARCHLISTCTRL wxID_HIGHEST + 667

// just to keep compiler happy
static wxCommandEvent nullEvent;

wxBEGIN_EVENT_TABLE(CSearchDlg, wxPanel)
	EVT_BUTTON(IDC_STARTS, CSearchDlg::OnBnClickedStart)
	EVT_TEXT_ENTER(IDC_SEARCHNAME, CSearchDlg::OnBnClickedStart)
	EVT_CHOICE(ID_SEARCHTYPE, CSearchDlg::OnSearchTypeChanged)

	EVT_BUTTON(IDC_CANCELS, CSearchDlg::OnBnClickedStop)
	EVT_BUTTON(IDC_SEARCHMORE, CSearchDlg::OnBnClickedSearchMore)

	EVT_LIST_ITEM_SELECTED(ID_SEARCHLISTCTRL, CSearchDlg::OnListItemSelected)

	EVT_BUTTON(IDC_SDOWNLOAD, CSearchDlg::OnBnClickedDownload)
	EVT_BUTTON(IDC_SEARCH_RESET, CSearchDlg::OnBnClickedReset)
	EVT_BUTTON(IDC_CLEAR_RESULTS, CSearchDlg::OnBnClickedClear)

	EVT_CHECKBOX(IDC_EXTENDEDSEARCHCHECK, CSearchDlg::OnExtendedSearchChange)
	EVT_CHECKBOX(IDC_FILTERCHECK, CSearchDlg::OnFilterCheckChange)

	EVT_MULENOTEBOOK_PAGE_CLOSING(ID_NOTEBOOK, CSearchDlg::OnSearchClosing)
	EVT_NOTEBOOK_PAGE_CHANGED(ID_NOTEBOOK, CSearchDlg::OnSearchPageChanged)

	// Event handlers for the parameter fields getting changed
	EVT_CUSTOM(wxEVT_TEXT, IDC_SEARCHNAME, CSearchDlg::OnFieldChanged)
	EVT_CUSTOM(wxEVT_TEXT, IDC_EDITSEARCHEXTENSION, CSearchDlg::OnFieldChanged)
	EVT_CUSTOM(wxEVT_SPINCTRL, wxID_ANY, CSearchDlg::OnFieldChanged)
	EVT_CUSTOM(wxEVT_CHOICE, wxID_ANY, CSearchDlg::OnFieldChanged)

	// Event handlers for the filter fields getting changed.
	EVT_TEXT_ENTER(ID_FILTER_TEXT, CSearchDlg::OnFilteringChange)
	EVT_CHECKBOX(ID_FILTER_INVERT, CSearchDlg::OnFilteringChange)
	EVT_CHECKBOX(ID_FILTER_KNOWN, CSearchDlg::OnFilteringChange)
	EVT_BUTTON(ID_FILTER, CSearchDlg::OnFilteringChange)
	EVT_BUTTON(ID_FILTER_RESET, CSearchDlg::OnFilterReset)
wxEND_EVENT_TABLE()

CSearchDlg::CSearchDlg(wxWindow *pParent)
: wxPanel(pParent, -1)
{
	m_last_search_time = 0;
	m_expiringSearchID = 0;
	m_inSearchClosing = false;

	wxSizer *content = searchDlg(this, true);
	content->Show(this, true);

	m_progressbar = CastChild(ID_SEARCHPROGRESS, wxGauge);
	m_progressbar->SetRange(100);

	m_notebook = CastChild(ID_NOTEBOOK, CMuleNotebook);

#ifdef __WXMAC__
	// #warning TODO: restore the image list if/when wxMac supports locating the image
#else
	// Initialise the image list
	wxImageList *m_ImageList = new wxImageList(16, 16);
	m_ImageList->Add(amuleSpecial(3));
	m_ImageList->Add(amuleSpecial(4));
	m_notebook->AssignImageList(m_ImageList);
#endif

	// Sanity sanity
	wxChoice *searchchoice = CastChild(ID_SEARCHTYPE, wxChoice);
	wxASSERT(searchchoice);
	wxASSERT(searchchoice->GetString(0) == _("Local"));
	wxASSERT(searchchoice->GetString(2) == _("Kad"));
	wxASSERT(searchchoice->GetCount() == 3);

	m_searchchoices = searchchoice->GetStrings();

	// Let's break it now.

	FixSearchTypes();

	CastChild(IDC_TypeSearch, wxChoice)->SetSelection(0);
	CastChild(IDC_SEARCHMINSIZE, wxChoice)->SetSelection(2);
	CastChild(IDC_SEARCHMAXSIZE, wxChoice)->SetSelection(2);

	// Not there initially.
	s_search_sizer->Show(s_extended_sizer, false);
	s_search_sizer->Show(s_filter_sizer, false);

	LoadSearchHistory();
	CastChild(IDC_SEARCHNAME, wxComboBox)
		->Bind(wxEVT_CONTEXT_MENU, &CSearchDlg::OnSearchNameContextMenu, this);

	Layout();
}

CSearchDlg::~CSearchDlg() {}

namespace
{
// eMule's CCustomAutoComplete default (amule-org/amule#643 review).
const int MAX_SEARCH_HISTORY_ENTRIES = 30;

// Search *query* history -- the strings typed into the Name field, not the
// results they returned (that's the separate, not-yet-implemented result
// persistence eMule does via StoredSearches.met; see #641). Deliberately
// not stored in amule.conf: query terms are arguably private, and putting
// them in the main config both bloats it and makes them travel with the
// file (config backups, the --amule-config-file push to amuleweb/amuleapi).
// A dedicated file mirrors eMule's own AC_SearchStrings.dat.
wxString SearchHistoryFilePath()
{
	return thePrefs::GetConfigDir() + "searchhistory.dat";
}
} // namespace

void CSearchDlg::LoadSearchHistory()
{
	wxComboBox *combo = CastChild(IDC_SEARCHNAME, wxComboBox);
	combo->Clear(); // item list, not the (empty at startup) text value

	CTextFile file;
	wxArrayString entries;
	if (file.Open(SearchHistoryFilePath(), CTextFile::read)) {
		// txtIgnoreEmptyLines|txtStripWhitespace has no single named
		// EReadTextFile enumerator to cast to (clang-tidy
		// clang-analyzer-optin.core.EnumCastOutOfRange, rightly --
		// EReadTextFile isn't declared as a flag enum), and
		// txtReadDefault also drops '#'-led lines, which would silently
		// eat a legitimate search term. Read unfiltered and do the
		// trim/empty-drop by hand instead.
		for (const wxString &line : file.ReadLines(txtReadAll)) {
			wxString trimmed = line;
			trimmed.Trim(true).Trim(false);
			if (!trimmed.IsEmpty()) {
				entries.Add(trimmed);
			}
		}
		file.Close();
	}

	size_t count = std::min<size_t>(entries.GetCount(), MAX_SEARCH_HISTORY_ENTRIES);
	for (size_t i = 0; i < count; ++i) {
		combo->Append(entries[i]);
	}
	// Feeds the dropdown's type-ahead suggestion (eMule's ACO_AUTOSUGGEST);
	// re-armed here and after every RecordSearchHistory/ClearSearchHistory
	// call so it always reflects the current entry set.
	combo->AutoComplete(entries);
}

void CSearchDlg::RecordSearchHistory(const wxString &term)
{
	if (!CPreferences::RememberSearchHistory() || term.IsEmpty()) {
		return;
	}

	wxComboBox *combo = CastChild(IDC_SEARCHNAME, wxComboBox);

	wxArrayString current;
	for (unsigned int i = 0; i < combo->GetCount(); ++i) {
		current.Add(combo->GetString(i));
	}
	wxArrayString entries = ApplySearchHistoryEntry(current, term, MAX_SEARCH_HISTORY_ENTRIES);

	combo->Clear();
	for (const wxString &entry : entries) {
		combo->Append(entry);
	}
	// combo->Clear() above only touches the item list, but set the value
	// back explicitly anyway so behaviour doesn't depend on that not
	// changing across wx versions/platforms.
	combo->SetValue(term);
	combo->AutoComplete(entries);

	CTextFile file;
	if (file.Open(SearchHistoryFilePath(), CTextFile::write)) {
		file.WriteLines(entries);
		file.Close();
	}
}

void CSearchDlg::ClearSearchHistory()
{
	if (wxFileExists(SearchHistoryFilePath())) {
		wxRemoveFile(SearchHistoryFilePath());
	}

	wxComboBox *combo = CastChild(IDC_SEARCHNAME, wxComboBox);
	combo->Clear();
	combo->AutoComplete(wxArrayString());
}

void CSearchDlg::OnSearchNameContextMenu(wxContextMenuEvent &WXUNUSED(evt))
{
	wxComboBox *combo = CastChild(IDC_SEARCHNAME, wxComboBox);

	// Overriding the field's context menu (to append the history action
	// below) must not silently drop the standard edit actions -- a plain
	// wxComboBox's only other context menu is this one, so without these
	// items right-clicking the field would offer no way to paste/copy
	// (amule-org/amule#643 review). Same custom-Paste-ID idiom as
	// CMuleTextCtrl::OnRightDown: wxMenu auto-enables Cut/Copy off Wx's own
	// selection tracking for the wxID_CUT/wxID_COPY stock IDs, but is too
	// permissive about wxID_PASTE (enabled even with an empty clipboard),
	// so Paste gets a custom ID and an explicit clipboard-content check.
	enum
	{
		ID_SEARCHNAME_PASTE = wxID_HIGHEST + 668
	};

	wxMenu menu;
	menu.Append(wxID_CUT);
	menu.Append(wxID_COPY);
	menu.Append(ID_SEARCHNAME_PASTE, _("Paste"));
	menu.Append(wxID_SELECTALL);

	bool canPaste = false;
	if (combo->CanPaste()) {
		if (wxTheClipboard->Open()) {
			if (wxTheClipboard->IsSupported(wxDF_TEXT)) {
				wxTextDataObject data;
				wxTheClipboard->GetData(data);
				canPaste = !data.GetText().IsEmpty();
			}
			wxTheClipboard->Close();
		}
	}
	menu.Enable(ID_SEARCHNAME_PASTE, canPaste);

	// Separator: "Clear" is a one-shot action, not another edit command --
	// keeping it visually apart avoids reading it as one of the group above.
	menu.AppendSeparator();
	wxMenuItem *clearItem = menu.Append(wxID_ANY, _("Clear search history"));
	clearItem->Enable(combo->GetCount() > 0);

	menu.Bind(wxEVT_MENU, [combo](wxCommandEvent &) { combo->Cut(); }, wxID_CUT);
	menu.Bind(wxEVT_MENU, [combo](wxCommandEvent &) { combo->Copy(); }, wxID_COPY);
	menu.Bind(wxEVT_MENU, [combo](wxCommandEvent &) { combo->Paste(); }, ID_SEARCHNAME_PASTE);
	menu.Bind(wxEVT_MENU, [combo](wxCommandEvent &) { combo->SetSelection(-1, -1); }, wxID_SELECTALL);
	menu.Bind(wxEVT_MENU, [this](wxCommandEvent &) { ClearSearchHistory(); }, clearItem->GetId());

	PopupMenu(&menu);
}

void CSearchDlg::FixSearchTypes()
{
	wxChoice *searchchoice = CastChild(ID_SEARCHTYPE, wxChoice);

	searchchoice->Clear();

	// We should have only filedonkey now. Let's insert stuff.

	int pos = 0;

	if (thePrefs::GetNetworkED2K()) {
		searchchoice->Insert(m_searchchoices[0], pos++);
		searchchoice->Insert(m_searchchoices[1], pos++);
	}

	if (thePrefs::GetNetworkKademlia()) {
		searchchoice->Insert(m_searchchoices[2], pos++);
	}

	// Restore the last-used search type (persisted in OnSearchTypeChanged)
	// instead of always defaulting to Local. The stored value is the stable
	// canonical code (0 = Local, 1 = Global, 2 = Kad); map it back onto
	// whichever entries are present now, falling back to the first entry when
	// the saved type's network is disabled (amule-org/amule#608).
	long savedType = 0;
	wxConfigBase::Get()->Read("/eMule/DefaultSearchType", &savedType, 0);
	int selection = 0;
	if (thePrefs::GetNetworkED2K()) {
		if (savedType == 1) { // Global
			selection = 1;
		} else if (savedType == 2 && thePrefs::GetNetworkKademlia()) { // Kad
			selection = 2;
		}
		// else Local (0), or the saved network is gone -> first entry
	}
	// With ED2K disabled the only entry is Kad at index 0, so 0 is correct.
	if (searchchoice->GetCount()) {
		if (selection >= (int)searchchoice->GetCount()) {
			selection = 0;
		}
		searchchoice->SetSelection(selection);
	}
}

int CSearchDlg::GetSelectedSearchTypeCanonical()
{
	int selection = CastChild(ID_SEARCHTYPE, wxChoice)->GetSelection();
	if (selection == wxNOT_FOUND) {
		return wxNOT_FOUND;
	}
	// FixSearchTypes() inserts choices as Local, Global, Kad, but drops the
	// ED2K pair when ED2K is disabled -- then the only entry (Kad) sits at 0,
	// so shift it onto the canonical Kad code (2).
	if (!thePrefs::GetNetworkED2K()) {
		selection += 2;
	}
	return selection;
}

void CSearchDlg::OnSearchTypeChanged(wxCommandEvent &WXUNUSED(evt))
{
	const int canonical = GetSelectedSearchTypeCanonical();
	if (canonical != wxNOT_FOUND) {
		wxConfigBase::Get()->Write("/eMule/DefaultSearchType", (long)canonical);
	}
}

CSearchListCtrl *CSearchDlg::GetSearchList(wxUIntPtr id)
{
	int nPages = m_notebook->GetPageCount();
	for (int i = 0; i < nPages; i++) {
		CSearchListCtrl *page = dynamic_cast<CSearchListCtrl *>(m_notebook->GetPage(i));

		if (page->GetSearchId() == id) {
			return page;
		}
	}

	return NULL;
}

void CSearchDlg::RekeySearch(wxUIntPtr oldID, wxUIntPtr newID)
{
	if (oldID == newID) {
		return;
	}
	if (CSearchListCtrl *page = GetSearchList(oldID)) {
		page->SetSearchId(newID);
	}
}

wxUIntPtr CSearchDlg::GetVisibleSearchId()
{
	int sel = m_notebook->GetSelection();
	if (sel == -1) {
		return 0;
	}
	CSearchListCtrl *page = dynamic_cast<CSearchListCtrl *>(m_notebook->GetPage(sel));
	return page ? page->GetSearchId() : 0;
}

void CSearchDlg::ApplyProgressToBar(uint32 status)
{
	const bool finished = (status == 0xffff || status == 0xfffe);
	if (finished) {
		// Finished (ed2k or Kad): reset the bar.
		m_progressbar->SetValue(0);
	} else {
		// Running: real global percent, or the Kad cosmetic ramp.
		UpdateProgress(status);
	}
	// Keep Stop in step with the visible tab's lifecycle — live while the search
	// runs, greyed once it finishes. Both callers feed this the visible tab's
	// status (RefreshVisibleTabProgress on monolithic, UpdateSearchProgress on
	// the remote GUI), so Stop follows the selected tab rather than the most-
	// recently-started search.
	FindWindow(IDC_CANCELS)->Enable(!finished);
}

#ifndef CLIENT_GUI
void CSearchDlg::RefreshVisibleTabProgress()
{
	wxUIntPtr sid = GetVisibleSearchId();
	// No tab => empty bar; otherwise reuse the same sentinel the EC PROGRESS
	// reply builds, so monolithic and the remote GUI stay in lockstep.
	ApplyProgressToBar(sid ? theApp->searchlist->GetSearchBarStatusById(sid) : 0xffff);
	// Keep the Kad-only "More" button in step with the visible tab: enabled only
	// while it is a running Kad search, greyed once the search completes
	// (IsKadSearch goes false when the Kad search ends). Refreshing it on the
	// same tick the bar updates is what makes it grey out on completion instead
	// of lingering until the next tab switch.
	FindWindow(IDC_SEARCHMORE)->Enable(sid && theApp->searchlist->IsKadSearch((uint32_t)sid));
}
#endif

void CSearchDlg::UpdateSearchProgress(uint32 searchID, uint32 status)
{
	// Cache per-tab so a tab switch can refresh the bar instantly.
	m_searchProgress[searchID] = status;

	if (status == 0xffff || status == 0xfffe) {
		// Search finished: clear this tab's Kad "!" marker (a no-op for an ed2k
		// tab, which has none) regardless of which tab is visible, so each tab
		// clears independently as its own search completes.
		KadSearchEnd(searchID);
	}
	// The single bottom bar tracks the visible tab only.
	if (searchID == GetVisibleSearchId()) {
		ApplyProgressToBar(status);
#ifdef CLIENT_GUI
		// Refresh the Kad-only "More" button from the per-search kind/lifecycle
		// the daemon just reported: IsKadSearch reads the remote cache that
		// CSearchListRem::HandlePacket updated immediately before this call.
		// (Monolithic drives the button from the local Kad layer on tab change.)
		FindWindow(IDC_SEARCHMORE)->Enable(theApp->searchlist->IsKadSearch((uint32_t)searchID));
#endif
	}
}

void CSearchDlg::AddResult(CSearchFile *toadd)
{
	CSearchListCtrl *outputwnd = GetSearchList(toadd->GetSearchID());

	if (outputwnd) {
		outputwnd->AddResult(toadd);

		// Update the result count
		UpdateHitCount(outputwnd);
	}
}

void CSearchDlg::UpdateResult(CSearchFile *toupdate)
{
	CSearchListCtrl *outputwnd = GetSearchList(toupdate->GetSearchID());

	if (outputwnd) {
		outputwnd->UpdateResult(toupdate);

		// Update the result count
		UpdateHitCount(outputwnd);
	}
}

void CSearchDlg::OnListItemSelected(wxListEvent &event)
{
	FindWindow(IDC_SDOWNLOAD)->Enable(true);

	event.Skip();
}

void CSearchDlg::OnExtendedSearchChange(wxCommandEvent &event)
{
	s_search_sizer->Show(s_extended_sizer, event.IsChecked());

	Layout();
}

void CSearchDlg::OnFilterCheckChange(wxCommandEvent &event)
{
	s_search_sizer->Show(s_filter_sizer, event.IsChecked());
	Layout();

	int nPages = m_notebook->GetPageCount();
	for (int i = 0; i < nPages; i++) {
		CSearchListCtrl *page = dynamic_cast<CSearchListCtrl *>(m_notebook->GetPage(i));

		page->EnableFiltering(event.IsChecked());

		UpdateHitCount(page);
	}
}

namespace
{
// Sets a bool for the lifetime of the scope. Local to this file: the one
// use is OnSearchClosing's re-entrancy guard, which is not worth a shared
// utility header.
class CScopedFlag
{
public:
	explicit CScopedFlag(bool &flag)
	: m_flag(flag)
	{
		m_flag = true;
	}
	~CScopedFlag() { m_flag = false; }
	CScopedFlag(const CScopedFlag &) = delete;
	CScopedFlag &operator=(const CScopedFlag &) = delete;

private:
	bool &m_flag;
};
} // namespace

void CSearchDlg::OnSearchClosing(wxBookCtrlEvent &evt)
{
	CSearchListCtrl *ctrl = dynamic_cast<CSearchListCtrl *>(m_notebook->GetPage(evt.GetSelection()));
	wxASSERT(ctrl);
	// Capture the ID *before* ShowResults(0), which sets m_nResultsID = 0 --
	// so every GetSearchId() after that line returns 0, and the stop/free
	// calls below silently addressed search 0 rather than this tab's search:
	// no EC_OP_SEARCH_STOP was ever sent, RemoveResults(0) freed nothing, and
	// m_searchProgress.erase(0) left the real entry behind. Predates the
	// multi-search work -- identical on master, ordering and all -- but it is
	// what made the daemon-side close gate look correct while the request it
	// gates never actually arrived (got3nks, PR #680 review).
	const wxUIntPtr searchID = ctrl->GetSearchId();
	// RemoveResults below fires MuleNotify::Search_Removed, which routes back
	// into CloseSearchTab for this very tab; the flag makes that a no-op
	// instead of a recursive close (see m_inSearchClosing). Scoped so it
	// clears on every exit path.
	CScopedFlag closingGuard(m_inSearchClosing);
	// Zero to avoid results added while destructing.
	ctrl->ShowResults(0);
	m_searchProgress.erase(searchID);
#ifdef CLIENT_GUI
	// Remote multi-search: closing a tab stops *and* frees that specific
	// search on the daemon (leaving other tabs' searches running). On a
	// legacy daemon this degrades to a parameterless stop of the single
	// search — which is the same "abort on close" behaviour as before.
	// Skipped when CloseSearchTab is the one driving this (DeletePage
	// fires this handler synchronously): the daemon already discarded
	// this id, so a stop request for it would be a wasted round trip
	// (got3nks, PR #680 review).
	if (searchID != m_expiringSearchID) {
		theApp->searchlist->StopSearchById(searchID, true);
	}
	m_expiringSearchID = 0;
#else
	// Monolithic: abort the global search if it was the last tab closed;
	// RemoveResults below stops any Kad search and frees the bucket in-process.
	if (evt.GetSelection() == ((int)m_notebook->GetPageCount() - 1)) {
		OnBnClickedStop(nullEvent);
	}
#endif
	theApp->searchlist->RemoveResults(searchID);

	// Do cleanups if this was the last tab
	if (m_notebook->GetPageCount() == 1) {
		FindWindow(IDC_SDOWNLOAD)->Enable(false);
		FindWindow(IDC_CLEAR_RESULTS)->Enable(false);
	}
}

void CSearchDlg::OnStartRejected(wxUIntPtr searchID, const wxString &error)
{
	// A rejected browse ("View Files") reaches this same path in amuleGUI --
	// SendBrowseRequest sends the same EC_TAG_SEARCH_REF and the daemon echoes
	// it on its failure exits too. It needs the tab dropped and the reason
	// shown, but NOT the search-button reset below: the user never pressed
	// Search, so clearing Download/Stop would disable them for whatever search
	// tab happens to be visible. Read the tab's kind before closing it.
	const CSearchListCtrl *ctrl = GetSearchList(searchID);
	const bool wasBrowse = ctrl && ctrl->IsBrowse();

	// Drop the tab the client optimistically created for a start that never
	// happened (no-op in the monolithic build, which creates none).
	CloseSearchTab(searchID);

	if (!error.IsEmpty()) {
		// Deferred off the current call stack: in amuleGUI this runs inside
		// CECSocket's reply handling, and wxMessageBox spins a nested event
		// loop that re-enters CECSocket::OnInput and clobbers its rx state --
		// the same hazard CAddLinkHandler documents. Harmless in the
		// monolithic build, so both go through the one path.
		//
		// `error` is captured BY VALUE, not by reference: it is a const& to
		// the caller's string and this body runs after that caller has
		// returned. (The capture is also why there is no named local copy --
		// performance-unnecessary-copy-initialization flags that, while the
		// copy itself is required.)
		const wxString title = wasBrowse ? _("ERROR") : _("Search warning");
		wxTheApp->CallAfter([error, title]() {
			wxMessageBox(error, title, wxOK | wxCENTRE | wxICON_INFORMATION);
		});
	}

	if (!wasBrowse) {
		// Back to the pre-search button state: the search never started, so
		// "Stop" must not stay armed for it.
		FindWindow(IDC_STARTS)->Enable();
		FindWindow(IDC_SDOWNLOAD)->Disable();
		FindWindow(IDC_CANCELS)->Disable();
	}
}

void CSearchDlg::CloseSearchTab(wxUIntPtr searchID)
{
	if (m_inSearchClosing) {
		// Already closing a tab: this is the monolithic close path calling
		// back into us (OnSearchClosing -> CSearchList::RemoveResults ->
		// MuleNotify::Search_Removed -> here) for the tab it is itself in
		// the middle of removing. That path does the whole job already.
		return;
	}
	CSearchListCtrl *ctrl = GetSearchList(searchID);
	if (!ctrl) {
		return; // no tab open for this id -- nothing to do
	}
	int nPages = (int)m_notebook->GetPageCount();
	for (int i = 0; i < nPages; i++) {
		if (m_notebook->GetPage(i) != ctrl) {
			continue;
		}
		// DeletePage fires PAGE_CLOSING synchronously (see MuleNotebook.cpp),
		// which re-enters OnSearchClosing on this same call stack and does
		// all the cleanup -- ShowResults(0), m_searchProgress.erase,
		// RemoveResults, last-tab button disabling -- itself. Setting this
		// first tells it to skip only the StopSearchById call: the core has
		// already discarded this id (amuleGUI got EC_TAG_SEARCH_EXPIRED for
		// it; monolithic freed the bucket), so a stop request for it would
		// be a wasted round trip (got3nks, PR #680 review).
		m_expiringSearchID = searchID;
		m_notebook->DeletePage(i);
		break;
	}
}

void CSearchDlg::OnSearchPageChanged(wxBookCtrlEvent &WXUNUSED(evt))
{
	int selection = m_notebook->GetSelection();

	// Workaround for a bug in wxWidgets, where deletions of pages
	// can result in an invalid selection. This has been reported as
	// http://sourceforge.net/tracker/index.php?func=detail&aid=1865141&group_id=9863&atid=109863
	if (selection >= (int)m_notebook->GetPageCount()) {
		selection = m_notebook->GetPageCount() - 1;
	}

	// Only enable the Download button for pages where files have been selected
	if (selection != -1) {
		CSearchListCtrl *ctrl = dynamic_cast<CSearchListCtrl *>(m_notebook->GetPage(selection));

		bool enable = (ctrl->GetSelectedItemCount() > 0);
		FindWindow(IDC_SDOWNLOAD)->Enable(enable);

		// Refresh the bottom bar instantly for the newly-visible tab so it
		// tracks the selected search rather than the last one that updated it.
#ifdef CLIENT_GUI
		// Remote GUI: from the per-search EC progress cache. If this tab has no
		// cached status yet (progress not polled, or daemon-side state lost
		// across an EC reconnect), clear the bar rather than leaving it frozen
		// on the previous tab's value — the next poll fills in the real state.
		if (!m_searchProgress.empty()) {
			std::map<wxUIntPtr, uint32>::const_iterator it =
				m_searchProgress.find(ctrl->GetSearchId());
			if (it != m_searchProgress.end()) {
				ApplyProgressToBar(it->second);
			} else {
				m_progressbar->SetValue(0);
			}
		}
#else
		// Monolithic: from the local core's per-search lifecycle.
		RefreshVisibleTabProgress();
#endif

		// "More" is Kad-only — enable when this tab's searchID still
		// corresponds to an active Kad search.
		FindWindow(IDC_SEARCHMORE)
			->Enable(theApp->searchlist->IsKadSearch((uint32_t)ctrl->GetSearchId()));
	} else {
		FindWindow(IDC_SEARCHMORE)->Enable(false);
	}
}

void CSearchDlg::OnBnClickedStart(wxCommandEvent &WXUNUSED(evt))
{
	if (!thePrefs::GetNetworkED2K() && !thePrefs::GetNetworkKademlia()) {
		wxMessageBox(_("It's impossible to search when both eD2k and Kademlia are disabled."),
			_("Search error"),
			wxOK | wxCENTRE | wxICON_ERROR);
		return;
	}

	// Debounce accidental double-clicks, but keep it short so multi-search
	// users can fire several searches (e.g. global + Kad) back-to-back.
	uint64 now = GetTickCount64();
	if ((now - m_last_search_time) > 500) {
		m_last_search_time = now;
		// Stop previous ED2K search state only (the server has a
		// single in-flight search packet per session and m_searchPacket
		// has to be reset).  Do NOT stop a previous Kad search: the
		// Kad data layer (CSearchManager::m_searches) supports multiple
		// concurrent searches keyed by target hash, and stopping the
		// previous one immediately deletes its CSearch (which strips
		// the "!" tab indicator and halts result delivery).
		// Unconditionally calling OnBnClickedStop here was the reason
		// starting a second Kad search appeared to cancel the first.
		theApp->searchlist->StopSearch(/*globalOnly=*/true);
		StartNewSearch();
	}
}

void CSearchDlg::OnFieldChanged(wxEvent &WXUNUSED(evt))
{
	bool enable = false;

	// These are the IDs of the search-fields
	int textfields[] = { IDC_SEARCHNAME, IDC_EDITSEARCHEXTENSION };

	for (int textfield : textfields) {
		enable |= !CastChild(textfield, wxTextEntry)->GetValue().IsEmpty();
	}

	// Check if either of the dropdowns have been changed
	enable |= (CastChild(IDC_SEARCHMINSIZE, wxChoice)->GetSelection() != 2);
	enable |= (CastChild(IDC_SEARCHMAXSIZE, wxChoice)->GetSelection() != 2);
	enable |= (CastChild(IDC_TypeSearch, wxChoice)->GetSelection() > 0);
	enable |= (CastChild(ID_AUTOCATASSIGN, wxChoice)->GetSelection() > 0);

	// These are the IDs of the search-fields
	int spinfields[] = { IDC_SPINSEARCHMIN, IDC_SPINSEARCHMAX, IDC_SPINSEARCHAVAILABILITY };
	for (int spinfield : spinfields) {
		enable |= (CastChild(spinfield, wxSpinCtrl)->GetValue() > 0);
	}

	// Enable the "Reset" button if any fields contain text
	FindWindow(IDC_SEARCH_RESET)->Enable(enable);

	// Enable the Server Search button if the Name field contains text
	enable = !CastChild(IDC_SEARCHNAME, wxTextEntry)->GetValue().IsEmpty();
	FindWindow(IDC_STARTS)->Enable(enable);
}

void CSearchDlg::OnFilteringChange(wxCommandEvent &WXUNUSED(evt))
{
	ApplyFilter();
}

void CSearchDlg::OnFilterReset(wxCommandEvent &WXUNUSED(evt))
{
	// Back to the state a fresh session starts in: empty expression, both
	// toggles off (issue #698). "Reset Fields" covers the search parameters
	// only, so before this there was no way to undo a filter except editing
	// each control by hand.
	CastChild(ID_FILTER_TEXT, wxTextCtrl)->Clear();
	CastChild(ID_FILTER_INVERT, wxCheckBox)->SetValue(false);
	CastChild(ID_FILTER_KNOWN, wxCheckBox)->SetValue(false);

	// SetValue()/Clear() do not emit the change events the filter normally
	// reacts to, so push the cleared state out explicitly -- otherwise the
	// controls would read as reset while the pages stayed filtered.
	ApplyFilter();
}

void CSearchDlg::ApplyFilter()
{
	wxString filter = CastChild(ID_FILTER_TEXT, wxTextCtrl)->GetValue();
	bool invert = CastChild(ID_FILTER_INVERT, wxCheckBox)->GetValue();
	bool known = CastChild(ID_FILTER_KNOWN, wxCheckBox)->GetValue();

	// Check that the expression compiles before we try to assign it
	// Otherwise we will get an error-dialog for each result-list.
	if (wxRegEx(filter, wxRE_DEFAULT | wxRE_ICASE).IsValid()) {
		int nPages = m_notebook->GetPageCount();
		for (int i = 0; i < nPages; i++) {
			CSearchListCtrl *page = dynamic_cast<CSearchListCtrl *>(m_notebook->GetPage(i));

			page->SetFilter(filter, invert, known);

			UpdateHitCount(page);
		}
	}
}

bool CSearchDlg::CheckTabNameExists(const wxString &searchString)
{
	int nPages = m_notebook->GetPageCount();
	for (int i = 0; i < nPages; i++) {
		// The BeforeLast(' ') is to strip the hit-count from the name
		if (m_notebook->GetPageText(i).BeforeLast(' ') == searchString) {
			return true;
		}
	}

	return false;
}

void CSearchDlg::CreateNewTab(const wxString &searchString, wxUIntPtr nSearchID)
{
	CSearchListCtrl *list = new CSearchListCtrl(
		m_notebook, ID_SEARCHLISTCTRL, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxNO_BORDER);
	m_notebook->AddPage(list, searchString, true, 0);

	// Ensure that new results are filtered
	bool enable = CastChild(IDC_FILTERCHECK, wxCheckBox)->GetValue();
	wxString filter = CastChild(ID_FILTER_TEXT, wxTextCtrl)->GetValue();
	bool invert = CastChild(ID_FILTER_INVERT, wxCheckBox)->GetValue();
	bool known = CastChild(ID_FILTER_KNOWN, wxCheckBox)->GetValue();

	list->SetFilter(filter, invert, known);
	list->EnableFiltering(enable);
	list->ShowResults(nSearchID);

	Layout();
	FindWindow(IDC_CLEAR_RESULTS)->Enable(true);

	// AddPage above made the new tab the selected one; defer to
	// IsKadSearch on its searchID so a freshly-created ED2K tab leaves
	// the button disabled.
	FindWindow(IDC_SEARCHMORE)->Enable(theApp->searchlist->IsKadSearch((uint32_t)nSearchID));
}

uint32 CSearchDlg::s_optimisticIdCounter = 0;

// Single source of the remote-GUI OPTIMISTIC placeholder tab id, used by both a
// new search (StartNewSearch) and a browse. A placeholder tab is created the
// instant the user acts, then rekeyed to the daemon's real search id when the
// START reply arrives (RemapSearch). The daemon allocates ed2k ids from the low
// quarter [1, 0x3fffffff] and Kad ids from the top half (>= 0x80000000), so a
// plain low placeholder could numerically equal an earlier tab's daemon id once
// the counters drift apart — RekeySearch would then match the WRONG tab and
// corrupt the tab map (searches silently stop). Reserving bit 30 puts every
// placeholder in [0x40000000, 0x7fffffff], a range no daemon allocator ever
// produces, so a placeholder can never collide with a daemon id — while staying
// bottom-half, clear of Kad. Search and browse share the one counter, so their
// placeholders never collide with each other either.
wxUIntPtr CSearchDlg::AllocateOptimisticId()
{
	s_optimisticIdCounter = (s_optimisticIdCounter + 1) & 0x7fffffff;
	return 0x40000000u | (s_optimisticIdCounter & 0x3fffffffu);
}

CSearchListCtrl *CSearchDlg::GetBrowseList(uint32 ecid, int *outPage)
{
	for (size_t i = 0; i < m_notebook->GetPageCount(); ++i) {
		CSearchListCtrl *page = dynamic_cast<CSearchListCtrl *>(m_notebook->GetPage(i));
		if (page && page->GetBrowseEcid() == ecid) {
			if (outPage) {
				*outPage = static_cast<int>(i);
			}
			return page;
		}
	}
	return nullptr;
}

bool CSearchDlg::ActivateBrowseTabIfOpen(uint32 peerEcid)
{
	// ecid 0 is "no live client" (e.g. an unlinked friend): nothing to match.
	if (peerEcid == 0) {
		return false;
	}
	int page = -1;
	if (GetBrowseList(peerEcid, &page) && page >= 0) {
		m_notebook->SetSelection(static_cast<size_t>(page));
		// The request came from another panel (Friends / Transfers); bring the
		// Search panel forward so the already-open tab is actually revealed.
		if (theApp->amuledlg) {
			theApp->amuledlg->ShowSearchWindow();
		}
		return true;
	}
	return false;
}

void CSearchDlg::EnsureBrowseTab(uint32 peerEcid, const wxString &userName, wxUIntPtr searchID)
{
	// A re-browse of the same peer refreshes the existing tab: rekey its
	// result-routing ID to the new one (the daemon may allocate a fresh ID) and
	// mark it browsing again, rather than opening a duplicate.
	if (CSearchListCtrl *page = GetBrowseList(peerEcid)) {
		page->SetSearchId(searchID);
		page->SetBrowseName(userName);
		page->SetBrowseStatus(BROWSE_IN_PROGRESS);
		UpdateHitCount(page);
		return;
	}

	CreateNewTab(userName, searchID);
	if (CSearchListCtrl *page = GetSearchList(searchID)) {
		page->SetBrowseEcid(peerEcid);
		page->SetBrowseName(userName);
		page->SetBrowseStatus(BROWSE_IN_PROGRESS);
		UpdateHitCount(page);
	}
	// A freshly-started browse comes from another panel (Friends / Transfers),
	// so bring the Search panel forward to reveal the new tab — including the
	// toolbar button state, not just the panel content. Only on tab creation,
	// never on a refresh or a streaming result, which would yank the panel out
	// from under the user.
	if (theApp->amuledlg) {
		theApp->amuledlg->ShowSearchWindow();
	}
}

void CSearchDlg::SetBrowseStatus(wxUIntPtr searchID, uint32 status)
{
	CSearchListCtrl *page = GetSearchList(searchID);
	if (page && page->IsBrowse()) {
		page->SetBrowseStatus(status);
		UpdateHitCount(page);
	}
}

void CSearchDlg::OnBnClickedStop(wxCommandEvent &WXUNUSED(evt))
{
	// Stop only the selected tab's search. The parameterless StopSearch() acts
	// on the scalar most-recently-started search (m_currentSearch), so with
	// several searches open it would stop the wrong tab whenever the visible one
	// is not the newest. Address the daemon/core by the visible tab's own id.
	wxUIntPtr sid = GetVisibleSearchId();
	if (sid) {
		theApp->searchlist->StopSearchById(sid);
	} else {
		theApp->searchlist->StopSearch();
	}
	ResetControls();
}

void CSearchDlg::OnBnClickedSearchMore(wxCommandEvent &WXUNUSED(evt))
{
	// "More" button: ask the currently-selected Kad search for more
	// results.  Uses CSearch::RequestMoreResults() (which dispatches
	// the existing KADEMLIA_FIND_VALUE_MORE wide-reask variant) to
	// widen the search frontier — peers we already queried return up
	// to 11 closer contacts instead of 2, and the existing
	// ProcessResponse cascade then queries the new neighbours with
	// FIND_VALUE.  Bounded per-search by KADEMLIA_FIND_VALUE_MORE_REASKS
	// (4) inside CSearch.
	//
	// For ED2K Local / Global searches there is currently no equivalent
	// — the "More" button silently no-ops on non-Kad tabs.  This mirrors
	// the prior behaviour of the (never-wired) ED2K-only stub.
	int sel = m_notebook->GetSelection();
	if (sel == -1) {
		return;
	}
	CSearchListCtrl *page = dynamic_cast<CSearchListCtrl *>(m_notebook->GetPage(sel));
	if (!page) {
		return;
	}
	const uint32_t searchID = (uint32_t)page->GetSearchId();
	// RequestMoreResults logs the outcome itself (single source of truth) — and
	// on amuleGUI that log is the daemon's, forwarded over EC, so the remote GUI
	// shows the real result rather than an optimistic guess.
	theApp->searchlist->RequestMoreResults(searchID);
}

void CSearchDlg::ResetControls()
{
	m_progressbar->SetValue(0);

	FindWindow(IDC_CANCELS)->Disable();
	FindWindow(IDC_STARTS)->Enable(!CastChild(IDC_SEARCHNAME, wxTextEntry)->GetValue().IsEmpty());
}

void CSearchDlg::LocalSearchEnd()
{
	ResetControls();
}

void CSearchDlg::KadSearchEnd(uint32 id)
{
	int nPages = m_notebook->GetPageCount();
	for (int i = 0; i < nPages; ++i) {
		CSearchListCtrl *page = dynamic_cast<CSearchListCtrl *>(m_notebook->GetPage(i));
		if (page->GetSearchId() == id || id == 0) { // 0: just update all pages (there is only one KAD
							    // search running at a time anyway)
			wxString rest;
			if (m_notebook->GetPageText(i).StartsWith("!", &rest)) {
				m_notebook->SetPageText(i, rest);
			}
		}
	}

	// If the search that just ended is the one currently shown, the
	// "More" button now has no candidate to widen — disable it.
	int sel = m_notebook->GetSelection();
	if (sel != -1) {
		CSearchListCtrl *page = dynamic_cast<CSearchListCtrl *>(m_notebook->GetPage(sel));
		if (page) {
			FindWindow(IDC_SEARCHMORE)
				->Enable(theApp->searchlist->IsKadSearch((uint32_t)page->GetSearchId()));
		}
	}
}

void CSearchDlg::OnBnClickedDownload(wxCommandEvent &WXUNUSED(evt))
{
	int sel = m_notebook->GetSelection();
	if (sel != -1) {
		CSearchListCtrl *list = dynamic_cast<CSearchListCtrl *>(m_notebook->GetPage(sel));

		// Download with items added to category specified in the drop-down menu
		list->DownloadSelected();
	}
}

void CSearchDlg::OnBnClickedClear(wxCommandEvent &WXUNUSED(ev))
{
	OnBnClickedStop(nullEvent);

	m_notebook->DeleteAllPages();

	FindWindow(IDC_CLEAR_RESULTS)->Enable(false);
	FindWindow(IDC_SDOWNLOAD)->Enable(false);
	FindWindow(IDC_SEARCHMORE)->Enable(false);
}

void CSearchDlg::StartNewSearch()
{
	FindWindow(IDC_STARTS)->Disable();
	FindWindow(IDC_SDOWNLOAD)->Disable();
	FindWindow(IDC_CANCELS)->Enable();

	CSearchList::CSearchParams params;

	params.searchString = CastChild(IDC_SEARCHNAME, wxTextEntry)->GetValue();
	params.searchString.Trim(true);
	params.searchString.Trim(false);

	if (params.searchString.IsEmpty()) {
		return;
	}

	RecordSearchHistory(params.searchString);

	if (CastChild(IDC_EXTENDEDSEARCHCHECK, wxCheckBox)->GetValue()) {
		params.extension = CastChild(IDC_EDITSEARCHEXTENSION, wxTextCtrl)->GetValue();

		uint32 sizemin = GetTypeSize((uint8)CastChild(IDC_SEARCHMINSIZE, wxChoice)->GetSelection());
		uint32 sizemax = GetTypeSize((uint8)CastChild(IDC_SEARCHMAXSIZE, wxChoice)->GetSelection());

		// Parameter Minimum Size
		params.minSize =
			(uint64_t)(CastChild(IDC_SPINSEARCHMIN, wxSpinCtrl)->GetValue()) * (uint64_t)sizemin;

		// Parameter Maximum Size
		params.maxSize =
			(uint64_t)(CastChild(IDC_SPINSEARCHMAX, wxSpinCtrl)->GetValue()) * (uint64_t)sizemax;

		if ((params.maxSize < params.minSize) && (params.maxSize)) {
			wxMessageDialog dlg(this,
				_("Min size must be smaller than max size. Max size ignored."),
				_("Search warning"),
				wxOK | wxCENTRE | wxICON_INFORMATION);
			dlg.ShowModal();

			params.maxSize = 0;
		}

		// Parameter Availability
		params.availability = CastChild(IDC_SPINSEARCHAVAILABILITY, wxSpinCtrl)->GetValue();

		switch (CastChild(IDC_TypeSearch, wxChoice)->GetSelection()) {
		case 0:
			params.typeText.Clear();
			break;
		case 1:
			params.typeText = ED2KFTSTR_ARCHIVE;
			break;
		case 2:
			params.typeText = ED2KFTSTR_AUDIO;
			break;
		case 3:
			params.typeText = ED2KFTSTR_CDIMAGE;
			break;
		case 4:
			params.typeText = ED2KFTSTR_IMAGE;
			break;
		case 5:
			params.typeText = ED2KFTSTR_PROGRAM;
			break;
		case 6:
			params.typeText = ED2KFTSTR_DOCUMENT;
			break;
		case 7:
			params.typeText = ED2KFTSTR_VIDEO;
			break;
		default:
			AddDebugLogLineC(logGeneral,
				CFormat("Warning! Unknown search-category (%s) selected!") % params.typeText);
			break;
		}
	}

	SearchType search_type = KadSearch;

	// Canonical order (0 = Local, 1 = Global, 2 = Kad), normalised for the
	// disabled-ED2K case inside the helper.
	int selection = GetSelectedSearchTypeCanonical();

	switch (selection) {
	case 0: // Local Search
		search_type = LocalSearch;
		break;
	case 1: // Global Search
		search_type = GlobalSearch;
		break;
	case 2: // Kad search
		search_type = KadSearch;
		break;
	default:
		// Should never happen
		wxFAIL;
		break;
	}

#ifdef CLIENT_GUI
	// Remote GUI: real_id is an OPTIMISTIC placeholder tab id. The tab is created
	// immediately (for instant feedback) and rekeyed to the daemon's real search
	// id when the START reply arrives (RemapSearch). AllocateOptimisticId is the
	// single source of both the value and its collision-free range — shared with
	// browse tabs so search + browse placeholders draw from one counter and never
	// collide with each other or with a daemon id before the rekey. See its
	// definition for why the range (bit 30) matters.
	uint32 real_id = static_cast<uint32>(AllocateOptimisticId());
#else
	// Monolithic: the id is used directly (no remap). Stay in the bottom half of
	// the uint32 space so it can never collide with Kad ids (>= 0x80000000).
	s_optimisticIdCounter = (s_optimisticIdCounter + 1) & 0x7fffffff;
	uint32 real_id = s_optimisticIdCounter;
#endif
	wxString error = theApp->searchlist->StartNewSearch(&real_id, search_type, params);
	if (!error.IsEmpty()) {
		// Search failed / Remote in progress. Shared with amuleGUI's
		// EC_OP_FAILED path so both builds report a rejected start the same
		// way (got3nks, PR #680 review). Note amuleGUI never reaches here:
		// CSearchListRem::StartNewSearch returns "" unconditionally and the
		// rejection arrives later over EC.
		OnStartRejected(real_id, error);
	} else {
		CreateNewTab(((search_type == KadSearch) ? "!" : "") + params.searchString + " (0)", real_id);
	}
}

void CSearchDlg::UpdateHitCount(CSearchListCtrl *page)
{
	for (uint32 i = 0; i < (uint32)m_notebook->GetPageCount(); ++i) {
		if (m_notebook->GetPage(i) == page) {
			size_t shown = page->GetItemCount();
			size_t hidden = page->GetHiddenItemCount();

			// A "View Files" tab composes its label from the stored peer name +
			// lifecycle marker (kept on the control), so the base name survives
			// re-labelling and never gets mangled by the BeforeLast(' ') strip
			// used for ordinary search tabs.
			if (page->IsBrowse()) {
				wxString count = hidden ? (CFormat(wxT("%u/%u")) % shown % (shown + hidden))
								  .GetString()
							: (CFormat(wxT("%u")) % shown).GetString();
				wxString label;
				switch (page->GetBrowseStatus()) {
				case BROWSE_FAILED:
					// Peer denied, went offline, or dropped mid-list. Reuse the
					// existing "Failed" catalog string (no new translatable string).
					label = CFormat(wxT("%s (%s)")) % page->GetBrowseName() % _("Failed");
					break;
				case BROWSE_FINISHED:
					label = CFormat(wxT("%s (%s)")) % page->GetBrowseName() % count;
					break;
				default: // BROWSE_IN_PROGRESS: trailing dots = still arriving.
					label = CFormat(wxT("%s (%s...)")) % page->GetBrowseName() % count;
					break;
				}
				m_notebook->SetPageText(i, label);
				break;
			}

			wxString searchtxt = m_notebook->GetPageText(i).BeforeLast(' ');
			if (!searchtxt.IsEmpty()) {
				if (hidden) {
					searchtxt += CFormat(" (%u/%u)") % shown % (shown + hidden);
				} else {
					searchtxt += CFormat(" (%u)") % shown;
				}

				m_notebook->SetPageText(i, searchtxt);
			}

			break;
		}
	}
}

void CSearchDlg::OnBnClickedReset(wxCommandEvent &WXUNUSED(evt))
{
	// wxTextEntry::Clear(), not wxComboBox's own Clear() -- the latter empties
	// the dropdown's item list (the history), not the typed value.
	CastChild(IDC_SEARCHNAME, wxTextEntry)->Clear();
	CastChild(IDC_EDITSEARCHEXTENSION, wxTextCtrl)->Clear();
	CastChild(IDC_SPINSEARCHMIN, wxSpinCtrl)->SetValue(0);
	CastChild(IDC_SEARCHMINSIZE, wxChoice)->SetSelection(2);
	CastChild(IDC_SPINSEARCHMAX, wxSpinCtrl)->SetValue(0);
	CastChild(IDC_SEARCHMAXSIZE, wxChoice)->SetSelection(2);
	CastChild(IDC_SPINSEARCHAVAILABILITY, wxSpinCtrl)->SetValue(0);
	CastChild(IDC_TypeSearch, wxChoice)->SetSelection(0);
	CastChild(ID_AUTOCATASSIGN, wxChoice)->SetSelection(0);

	FindWindow(IDC_SEARCH_RESET)->Enable(false);
}

void CSearchDlg::UpdateCatChoice()
{
	wxChoice *c_cat = CastChild(ID_AUTOCATASSIGN, wxChoice);
	c_cat->Clear();

	c_cat->Append(_("Main"));

	for (unsigned i = 1; i < theApp->glob_prefs->GetCatCount(); i++) {
		c_cat->Append(theApp->glob_prefs->GetCategory(i)->title);
	}

	c_cat->SetSelection(0);
}

void CSearchDlg::UpdateProgress(uint32 new_value)
{
	m_progressbar->SetValue(new_value);
}
// File_checked_for_headers
