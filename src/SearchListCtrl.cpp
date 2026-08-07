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

#include "SearchListCtrl.h" // Interface declarations

#include <algorithm> // Needed for std::find, std::min, std::sort
#include <vector>    // Needed for std::vector

#include <common/MenuIDs.h>
#include <common/Format.h> // Needed for CFormat
#include <tags/FileTags.h> // Needed for FT_MEDIA_LENGTH / _BITRATE / _CODEC

#include "amule.h"            // Needed for theApp
#include "Server.h"           // Needed for CServer
#include "ServerConnect.h"    // Needed for CServerConnect
#include "SearchList.h"       // Needed for CSearchFile
#include "SearchListModel.h"  // Needed for CSearchListModel
#include "GetTickCount.h"     // Needed for GetTickCount64()
#include "CommentDialogLst.h" // Needed for CCommentDialogLst (Kad comments/ratings)
#include "SearchDlg.h"        // Needed for CSearchDlg
#include "amuleDlg.h"         // Needed for CamuleDlg
#ifndef CLIENT_GUI
#include "TransferWnd.h"      // Needed for CTransferWnd (download-list batching)
#include "DownloadListCtrl.h" // Needed for CDownloadListCtrl (download-list batching)
#endif
#include "muuli_wdr.h"      // Needed for IDC_* / ID_* control ids
#include "OtherFunctions.h" // Needed for CmpAny, CastItoXBytes, GetFiletypeByName, ...
#include "Preferences.h"    // Needed for thePrefs
#include "GuiEvents.h"      // Needed for CoreNotify_Search_Add_Download

wxBEGIN_EVENT_TABLE(CSearchListCtrl, CMuleDataViewCtrl)
	EVT_DATAVIEW_ITEM_CONTEXT_MENU(wxID_ANY, CSearchListCtrl::OnRightClick)
	EVT_DATAVIEW_ITEM_ACTIVATED(wxID_ANY, CSearchListCtrl::OnItemActivated)
	EVT_DATAVIEW_SELECTION_CHANGED(wxID_ANY, CSearchListCtrl::OnSelectionChanged)

	EVT_MENU(MP_GETED2KLINK, CSearchListCtrl::OnPopupGetUrl)
	EVT_MENU(MP_RAZORSTATS, CSearchListCtrl::OnRazorStatsCheck)
	EVT_MENU(MP_SEARCHRELATED, CSearchListCtrl::OnRelatedSearch)
	EVT_MENU(MP_GETCOMMENTS, CSearchListCtrl::OnGetComments)
	EVT_MENU(MP_RESUME, CSearchListCtrl::OnPopupDownload)
	EVT_MENU_RANGE(MP_ASSIGNCAT, MP_ASSIGNCAT + 99, CSearchListCtrl::OnPopupDownload)
wxEND_EVENT_TABLE()

std::list<CSearchListCtrl *> CSearchListCtrl::s_lists;

// MLOrder-compatible bit values, reused from CMuleListCtrl so the persisted
// "TableOrderingSearch" config entries stay wire-compatible (see
// ListColumnStore.cpp, which already hardcodes these same values).
namespace
{
const unsigned SORT_DES = 0x1000;
const unsigned SORT_ALT = 0x2000;
const unsigned SORTING_MASK = 0x3000;
} // namespace

CSearchListCtrl::CSearchListCtrl(
	wxWindow *parent, wxWindowID winid, const wxPoint &pos, const wxSize &size, const wxString &name)
: CMuleDataViewCtrl(parent, winid, pos, size, 0, name)
, m_nResultsID(0)
, m_browseEcid(0)
, m_browseStatus(0)
, m_filterKnown(false)
, m_invert(false)
, m_filterEnabled(false)
{
	// Without this, idle events aren't guaranteed to reach this specific
	// window (wx's default idle-processing mode only visits windows opted
	// in this way), so OnIdle's column-resize detection -- the only way
	// this control learns about a user drag-resize, since there is no
	// portable wxDataViewCtrl "column resized" event -- would silently
	// never fire.
	SetExtraStyle(GetExtraStyle() | wxWS_EX_PROCESS_IDLE);

	m_model = new CSearchListModel(this);
	AssociateModel(m_model);
	m_model->DecRef(); // the control now holds the only reference

	AddTextColumn(_("File Name"),
		CSearchListModel::COL_NAME,
		"N",
		500,
		wxALIGN_LEFT,
		wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
	AddTextColumn(_("Size"),
		CSearchListModel::COL_SIZE,
		"Z",
		100,
		wxALIGN_LEFT,
		wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
	AddTextColumn(_("Sources"),
		CSearchListModel::COL_SOURCES,
		"u",
		50,
		wxALIGN_LEFT,
		wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
	AddTextColumn(_("Type"),
		CSearchListModel::COL_TYPE,
		"Y",
		65,
		wxALIGN_LEFT,
		wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
	// Rating: smiley icon + text label in one cell.
	AddIconTextColumn(_("Rating"),
		CSearchListModel::COL_RATING,
		"R",
		120,
		wxALIGN_LEFT,
		wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
	AddTextColumn(_("FileID"),
		CSearchListModel::COL_FILEID,
		"I",
		280,
		wxALIGN_LEFT,
		wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
	AddTextColumn(_("Status"),
		CSearchListModel::COL_STATUS,
		"S",
		100,
		wxALIGN_LEFT,
		wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
	// Media tag columns: ed2k/Kad publishers (eMule, eMule AI, aMule) can
	// advertise per-file media metadata in FT_MEDIA_LENGTH / _BITRATE /
	// _CODEC. Cells stay empty for non-media results.
	AddTextColumn(_("Length"),
		CSearchListModel::COL_LENGTH,
		"L",
		80,
		wxALIGN_LEFT,
		wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
	AddTextColumn(_("Bitrate"),
		CSearchListModel::COL_BITRATE,
		"B",
		80,
		wxALIGN_LEFT,
		wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
	AddTextColumn(_("Codec"),
		CSearchListModel::COL_CODEC,
		"C",
		80,
		wxALIGN_LEFT,
		wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
	// Directories is almost always empty (only populated when the result
	// came from a "view shared files" request, rare in practice), so put
	// it at the end with the other usually-empty columns.
	AddTextColumn(_("Directories"), // I would have preferred "Directory" but this is already translated
		CSearchListModel::COL_DIRECTORY,
		"D",
		280,
		wxALIGN_LEFT,
		wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);

	// Absorbs the macOS trailing-column sizing; the model answers COL_SPACER
	// with an empty string.
	AppendSpacerColumn(CSearchListModel::COL_SPACER);

	// Default sort is by name, ascending.
	m_sort_orders.emplace_back(CSearchListModel::COL_NAME, 0);
	GetColumn(CSearchListModel::COL_NAME)->SetSortOrder(true);

	// Only load settings for first list, otherwise sync with current lists
	if (s_lists.empty()) {
		m_columnStore.SetTableName("Search");
		LoadColumnSettings();
		m_columnStore.SetTableName("");
	} else {
		SyncLists(s_lists.front(), this);
	}

	InitColumnState();

	s_lists.push_back(this);
}

CSearchListCtrl::~CSearchListCtrl()
{
	// Push this list's current widths/sort state onward before it's gone,
	// so whichever tab happens to be closed *last* -- the one that
	// actually gets to SaveColumnSettings() below -- reflects the most
	// recently touched state, regardless of which tab the user last
	// resized/re-sorted. Doesn't depend on the idle-driven live sync
	// (CSearchListCtrl::OnIdle) ever having fired: that only keeps
	// multiple simultaneously-open tabs visually in sync as a UX nicety,
	// it's not what persistence correctness relies on here.
	SyncOtherLists(this);

	s_lists.remove(this);

	// We only save the settings if the last list was closed
	if (s_lists.empty()) {
		m_columnStore.SetTableName("Search");
		SaveColumnSettings();
	}
}

bool CSearchListCtrl::PassesFilter(const CSearchFile *file) const
{
	return IsFiltered(file);
}

bool CSearchListCtrl::IsFiltered(const CSearchFile *file) const
{
	// By default, everything is displayed. (Name kept from the original
	// wxListCtrl-era code -- despite the name, true means "passes the
	// filter, should be shown".)
	bool result = true;

	if (m_filterEnabled && m_filter.IsValid()) {
		result = m_filter.Matches(file->GetFileName().GetPrintable());
		result = ((result && !m_invert) || (!result && m_invert));
		if (result && m_filterKnown) {
			// Still a live status test, so results that were already known
			// stay hidden -- but never for the ones the user just queued
			// from this list, which would otherwise disappear under the
			// click that queued them (see m_userQueued).
			const bool queuedHere = m_userQueued.count(file->GetFileHash()) != 0;
			result = queuedHere || file->GetDownloadStatus() == CSearchFile::NEW;
		}
	}

	return result;
}

bool CSearchListCtrl::ShouldShow(const CSearchFile *file) const
{
	if (IsFiltered(file)) {
		return true;
	}
	// A parent that doesn't itself pass the filter is still shown as a
	// container if at least one child does -- regardless of whether it's
	// currently expanded. (This is a deliberate, flagged behaviour change
	// from the old hand-drawn-tree version, which only kept such a parent
	// visible while its children were already expanded-shown; with a real
	// tree control there's no reason to couple filtering to transient
	// expand state, and always surfacing the container is more discoverable.)
	const CSearchResultList &children = file->GetChildren();
	for (const CSearchFile *child : children) {
		if (IsFiltered(child)) {
			return true;
		}
	}
	return false;
}

void CSearchListCtrl::AddResult(CSearchFile *toshow)
{
	wxCHECK_RET(toshow->GetSearchID() == m_nResultsID, "Wrong search-id for result-list");
	m_model->NotifyFileAdded(toshow);
}

void CSearchListCtrl::UpdateResult(CSearchFile *toupdate)
{
	m_model->NotifyFileUpdated(toupdate);
}

void CSearchListCtrl::ShowResults(wxUIntPtr ResultsID)
{
	m_nResultsID = ResultsID;
	// Different result set entirely; nothing kept for the old one applies.
	m_userQueued.clear();
	m_model->NotifyFilterChanged(); // full reset: new search-id, entirely different result set
}

void CSearchListCtrl::SetFilter(const wxString &regExp, bool invert, bool filterKnown)
{
	m_filterText = regExp.IsEmpty() ? wxString(".*") : regExp;
	m_filter.Compile(m_filterText, wxRE_DEFAULT | wxRE_ICASE);
	m_filterKnown = filterKnown;
	m_invert = invert;
	// Re-applying the filter is the point at which the user asked to see the
	// list filtered afresh, so the rows held over from earlier downloads
	// collapse away here rather than lingering for the rest of the session.
	m_userQueued.clear();

	if (m_filterEnabled) {
		m_model->NotifyFilterChanged();
	}
}

void CSearchListCtrl::EnableFiltering(bool enabled)
{
	if (enabled != m_filterEnabled) {
		m_filterEnabled = enabled;
		m_model->NotifyFilterChanged();
	}
}

size_t CSearchListCtrl::GetHiddenItemCount() const
{
	if (!m_nResultsID) {
		return 0;
	}
	size_t hidden = 0;
	const CSearchResultList &results = theApp->searchlist->GetSearchResults(m_nResultsID);
	// Only top-level results are indexed (see CSearchResultIndex), so this
	// counts exactly the results the list would show but for the filter. A
	// grouped child is never "hidden" in its own right: it is reached through
	// its parent, which surfaces as a container for it.
	for (CSearchFile *file : results) {
		if (!ShouldShow(file)) {
			++hidden;
		}
	}
	return hidden;
}

size_t CSearchListCtrl::GetItemCount() const
{
	if (!m_nResultsID) {
		return 0;
	}
	size_t shown = 0;
	const CSearchResultList &results = theApp->searchlist->GetSearchResults(m_nResultsID);
	for (CSearchFile *file : results) {
		if (!file->GetParent() && ShouldShow(file)) {
			++shown;
			if (file->HasChildren() && IsExpanded(CSearchListModel::ToItem(file))) {
				const CSearchResultList &children = file->GetChildren();
				for (const CSearchFile *child : children) {
					if (IsFiltered(child)) {
						++shown;
					}
				}
			}
		}
	}
	return shown;
}

int CSearchListCtrl::GetSelectedItemCount() const
{
	wxDataViewItemArray selections;
	return static_cast<int>(const_cast<CSearchListCtrl *>(this)->GetSelections(selections));
}

int CSearchListCtrl::CompareFilesByColumn(
	const CSearchFile *f1, const CSearchFile *f2, unsigned column, bool alt, int modifier) const
{
	switch (column) {
	case CSearchListModel::COL_NAME:
		return modifier * CmpAny(f1->GetFileName(), f2->GetFileName());

	case CSearchListModel::COL_SIZE:
		return modifier * CmpAny(f1->GetFileSize(), f2->GetFileSize());

	case CSearchListModel::COL_SOURCES: {
		int cmp = CmpAny(f1->GetSourceCount(), f2->GetSourceCount());
		int cmp2 = CmpAny(f1->GetCompleteSourceCount(), f2->GetCompleteSourceCount());
		if (alt) {
			std::swap(cmp, cmp2);
		}
		if (cmp == 0) {
			cmp = cmp2;
		}
		return modifier * cmp;
	}

	case CSearchListModel::COL_TYPE: {
		int result = GetFiletypeByName(f1->GetFileName()).Cmp(GetFiletypeByName(f2->GetFileName()));
		if (result == 0) {
			result = CmpAny(f1->GetFileName().GetExt(), f2->GetFileName().GetExt());
		}
		return modifier * result;
	}

	case CSearchListModel::COL_RATING: {
		int r1 = f1->HasRating() ? f1->UserRating() : 0;
		int r2 = f2->HasRating() ? f2->UserRating() : 0;
		if (!r1 && !r2) {
			return 0;
		}
		if (!r1) {
			return 1; // unrated always sorts last, direction-independent
		}
		if (!r2) {
			return -1;
		}
		return modifier * CmpAny(r1, r2);
	}

	case CSearchListModel::COL_FILEID:
		return modifier * CmpAny(f2->GetFileHash(), f1->GetFileHash());

	case CSearchListModel::COL_STATUS:
		return modifier * CmpAny(DetermineStatusPrintable(const_cast<CSearchFile *>(f2)),
					  DetermineStatusPrintable(const_cast<CSearchFile *>(f1)));

	case CSearchListModel::COL_DIRECTORY: {
		int result = CmpAny(f1->GetDirectory(), f2->GetDirectory());
		if (result == 0) {
			result = CmpAny(f1->GetFileName(), f2->GetFileName());
		}
		return modifier * result;
	}

	case CSearchListModel::COL_LENGTH: {
		uint32 v1 = f1->GetIntTagValue(FT_MEDIA_LENGTH);
		uint32 v2 = f2->GetIntTagValue(FT_MEDIA_LENGTH);
		if (!v1 && !v2) {
			return 0;
		}
		if (!v1) {
			return 1;
		}
		if (!v2) {
			return -1;
		}
		return modifier * CmpAny(v1, v2);
	}

	case CSearchListModel::COL_BITRATE: {
		uint32 v1 = f1->GetIntTagValue(FT_MEDIA_BITRATE);
		uint32 v2 = f2->GetIntTagValue(FT_MEDIA_BITRATE);
		if (!v1 && !v2) {
			return 0;
		}
		if (!v1) {
			return 1;
		}
		if (!v2) {
			return -1;
		}
		return modifier * CmpAny(v1, v2);
	}

	case CSearchListModel::COL_CODEC: {
		const wxString c1 = FormatMediaCodec(f1->GetStrTagValue(FT_MEDIA_CODEC));
		const wxString c2 = FormatMediaCodec(f2->GetStrTagValue(FT_MEDIA_CODEC));
		if (c1.IsEmpty() && c2.IsEmpty()) {
			return 0;
		}
		if (c1.IsEmpty()) {
			return 1;
		}
		if (c2.IsEmpty()) {
			return -1;
		}
		return modifier * CmpAny(c1, c2);
	}
	}

	return 0;
}

int CSearchListCtrl::CompareFiles(const CSearchFile *f1, const CSearchFile *f2) const
{
	return CompareItems(CSearchListModel::ToItem(f1), CSearchListModel::ToItem(f2));
}

bool CSearchListCtrl::AltSortAllowed(unsigned column) const
{
	return column == CSearchListModel::COL_SOURCES;
}

void CSearchListCtrl::SyncLists(CSearchListCtrl *src, CSearchListCtrl *dst)
{
	wxCHECK_RET(src && dst, "NULL argument in SyncLists");

	for (unsigned i = 0; i < src->RealColumnCount(); ++i) {
		// Hidden state has to travel with the width: copying width alone
		// would leave the other tabs showing a column this one has hidden.
		const int col = static_cast<int>(i);
		const bool hidden = src->IsColumnHidden(col);
		if (dst->IsColumnHidden(col) != hidden) {
			dst->SetColumnHidden(col, hidden, src->GetColumn(i)->GetWidth());
		}
		if (!hidden && dst->GetColumn(i)->GetWidth() != src->GetColumn(i)->GetWidth()) {
			dst->GetColumn(i)->SetWidth(src->GetColumn(i)->GetWidth());
		}
	}
	dst->UpdateExpanderColumn();

	if (dst->m_sort_orders.empty() || src->m_sort_orders.empty() ||
		dst->m_sort_orders.front() != src->m_sort_orders.front()) {
		dst->m_sort_orders = src->m_sort_orders;
		if (!dst->m_sort_orders.empty()) {
			const CColPair &primary = dst->m_sort_orders.front();
			for (unsigned i = 0; i < dst->RealColumnCount(); ++i) {
				if (i != primary.first && dst->GetColumn(i)->IsSortKey()) {
					dst->GetColumn(i)->UnsetAsSortKey();
				}
			}
			dst->GetColumn(primary.first)->SetSortOrder(!(primary.second & SORT_DES));
			dst->GetModel()->Resort();
		}
	}
}

void CSearchListCtrl::SyncOtherLists(CSearchListCtrl *src)
{
	for (CSearchListCtrl *list : s_lists) {
		if (list != src) {
			SyncLists(src, list);
		}
	}
}

int CSearchListCtrl::CompareByColumn(const wxDataViewItem &item1,
	const wxDataViewItem &item2,
	unsigned column,
	bool alt,
	int modifier) const
{
	return CompareFilesByColumn(
		CSearchListModel::ToFile(item1), CSearchListModel::ToFile(item2), column, alt, modifier);
}

void CSearchListCtrl::GetDisplayOrder(wxDataViewItemArray &ordered) const
{
	std::vector<CSearchFile *> files;
	BuildDisplayOrder(files);

	ordered.Clear();
	ordered.Alloc(files.size());
	for (CSearchFile *file : files) {
		ordered.Add(CSearchListModel::ToItem(file));
	}
}

wxString CSearchListCtrl::GetRowLabel(const wxDataViewItem &item) const
{
	return CSearchListModel::ToFile(item)->GetFileName().GetPrintable();
}

wxString CSearchListCtrl::GetOldColumnOrder() const
{
	return "N,Z,u,Y,I,S";
}

void CSearchListCtrl::OnIdleHook()
{
	// One coalesced rebuild per idle for everything that arrived since the
	// last one (got3nks, PR #796 review): mixing incremental Item*
	// notifications with the full model reset a group formation needs left
	// wxGTK's tree inconsistent, and neither ItemChanged() nor a
	// delete-and-re-add worked around it -- only wxDataViewModel::Cleared()
	// reliably makes the control re-derive container-ness. Cleared() throws
	// away the control's own view state, so selection and expansion are
	// captured and re-applied around it -- otherwise a result landing
	// mid-search would deselect whatever the user had picked. Items are
	// CSearchFile*, still valid across the rebuild; ones that went away are
	// dropped by re-checking membership against the live tree afterwards.
	if (m_model->HasPending()) {
		wxDataViewItemArray selected;
		GetSelections(selected);
		wxDataViewItemArray expanded;
		{
			wxDataViewItemArray roots;
			m_model->GetChildren(wxDataViewItem(), roots);
			for (size_t i = 0; i < roots.GetCount(); ++i) {
				if (IsExpanded(roots[i])) {
					expanded.Add(roots[i]);
				}
			}
		}

		m_model->FlushPending();

		wxDataViewItemArray live;
		m_model->GetChildren(wxDataViewItem(), live);
		for (size_t i = 0; i < expanded.GetCount(); ++i) {
			if (live.Index(expanded[i]) != wxNOT_FOUND) {
				Expand(expanded[i]);
			}
		}
		wxDataViewItemArray restore;
		for (size_t i = 0; i < selected.GetCount(); ++i) {
			if (live.Index(selected[i]) != wxNOT_FOUND) {
				restore.Add(selected[i]);
			}
		}
		if (!restore.IsEmpty()) {
			SetSelections(restore);
		}
	}
}

void CSearchListCtrl::OnColumnWidthsChanged()
{
	// The base persists the new widths; this list additionally mirrors them to
	// the other open search tabs.
	CMuleDataViewCtrl::OnColumnWidthsChanged();
	SyncOtherLists(this);
}

void CSearchListCtrl::OnSortingChanged()
{
	if (GetModel()) {
		GetModel()->Resort();
	}
	SyncOtherLists(this);
}

void CSearchListCtrl::OnRightClick(wxDataViewEvent &event)
{
	if (GetSelectedItemCount()) {
		// No title -- see the identical rationale in the pre-port version
		// (wxMenu's title parameter renders inconsistently or not at all
		// as a context-popup header across platforms, issue #767).
		wxMenu menu;
		menu.Append(MP_RESUME, _("Download"));

		wxMenu *cats = new wxMenu(_("Category"));
		cats->Append(MP_ASSIGNCAT, _("Main"));
		for (unsigned i = 1; i < theApp->glob_prefs->GetCatCount(); i++) {
			cats->Append(MP_ASSIGNCAT + static_cast<int>(i),
				theApp->glob_prefs->GetCategory(i)->title);
		}

		menu.Append(MP_MENU_CATS, _("Download in category"), cats);
		menu.AppendSeparator();

		const wxString &statsServer = thePrefs::GetStatsServerName();
		if (!statsServer.IsEmpty()) {
			menu.Append(MP_RAZORSTATS, CFormat(_("Get %s for this file")) % statsServer);
			menu.AppendSeparator();
		}

		menu.Append(MP_SEARCHRELATED, _("Search related files (eD2k, local server)"));
		menu.Append(MP_GETCOMMENTS, _("Show all comments"));
		menu.AppendSeparator();
		menu.Append(MP_GETED2KLINK, _("Copy eD2k link to clipboard"));

		const bool enable = (GetSelectedItemCount() == 1);
		menu.Enable(MP_GETED2KLINK, enable);
		menu.Enable(MP_GETCOMMENTS, enable);
		menu.Enable(MP_MENU_CATS, (theApp->glob_prefs->GetCatCount() > 1));

		PopupMenu(&menu);
	} else {
		event.Skip();
	}
}

void CSearchListCtrl::OnItemActivated(wxDataViewEvent &event)
{
	CSearchFile *file = CSearchListModel::ToFile(event.GetItem());
	if (!file) {
		return;
	}
	if (file->HasChildren()) {
		if (IsExpanded(event.GetItem())) {
			Collapse(event.GetItem());
		} else {
			Expand(event.GetItem());
		}
	} else {
		DownloadSelected();
	}
}

void CSearchListCtrl::OnSelectionChanged(wxDataViewEvent &WXUNUSED(event))
{
	// Bubbles up like the old EVT_LIST_ITEM_SELECTED so
	// CSearchDlg::OnListItemSelected can enable the Download button; bound
	// separately on CSearchDlg for wxEVT_DATAVIEW_SELECTION_CHANGED.
}

void CSearchListCtrl::OnPopupGetUrl(wxCommandEvent &WXUNUSED(event))
{
	wxString URIs;
	wxDataViewItemArray selections;
	GetSelections(selections);
	for (const wxDataViewItem &item : selections) {
		CSearchFile *file = CSearchListModel::ToFile(item);
		URIs += theApp->CreateED2kLink(file) + "\n";
	}
	if (!URIs.IsEmpty()) {
		theApp->CopyTextToClipboard(URIs.RemoveLast());
	}
}

void CSearchListCtrl::OnGetComments(wxCommandEvent &WXUNUSED(event))
{
	wxDataViewItemArray selections;
	GetSelections(selections);
	if (selections.empty()) {
		return;
	}
	CSearchFile *file = CSearchListModel::ToFile(selections[0]);
	if (file) {
		// Same dialog the download list uses; its "Get from Kad" button drives
		// the on-demand community ratings/comments lookup for this result.
		CCommentDialogLst dialog(this, file);
		dialog.ShowModal();
	}
}

void CSearchListCtrl::OnRazorStatsCheck(wxCommandEvent &WXUNUSED(event))
{
	wxDataViewItemArray selections;
	GetSelections(selections);
	if (selections.empty()) {
		return;
	}
	CSearchFile *file = CSearchListModel::ToFile(selections[0]);
	theApp->amuledlg->LaunchUrl(thePrefs::GetStatsServerURL() + file->GetFileHash().Encode());
}

void CSearchListCtrl::OnRelatedSearch(wxCommandEvent &WXUNUSED(event))
{
	wxDataViewItemArray selections;
	GetSelections(selections);
	if (selections.empty()) {
		return;
	}

	if (thePrefs::GetNetworkED2K() && theApp->serverconnect->GetCurrentServer() != NULL &&
		theApp->serverconnect->GetCurrentServer()->GetRelatedSearchSupport()) {

		theApp->searchlist->StopSearch(true);
		theApp->amuledlg->m_searchwnd->ResetControls();
		wxString keyword("related");
		for (const wxDataViewItem &item : selections) {
			CSearchFile *file = CSearchListModel::ToFile(item);
			keyword << "::" << file->GetFileHash().Encode();
		}
		CastByID(IDC_SEARCHNAME, theApp->amuledlg->m_searchwnd, wxTextEntry)->SetValue(keyword);
		wxChoice *searchtype = CastByID(ID_SEARCHTYPE, theApp->amuledlg->m_searchwnd, wxChoice);
		searchtype->SetSelection(searchtype->FindString(_("Local")));
		theApp->amuledlg->m_searchwnd->StartNewSearch();
	} else {
		wxMessageBox(_("You are not currently connected to a server supporting the Related Files "
			       "search function"),
			_("Search error"),
			wxOK | wxCENTRE | wxICON_ERROR);
	}
}

void CSearchListCtrl::BuildDisplayOrder(std::vector<CSearchFile *> &ordered) const
{
	// The model yields top-level rows in arrival order, so they are put
	// through this list's own comparator -- the one CSearchListModel::
	// Compare() uses -- to match what is actually on screen under the
	// current sort. GetItemByRow()/GetRowByItem() would be the direct
	// route but exist only in wx's generic implementation, not on GTK or
	// macOS.
	const auto byDisplayOrder = [this](const CSearchFile *f1, const CSearchFile *f2) {
		return CompareFiles(f1, f2) < 0;
	};

	wxDataViewItemArray roots;
	m_model->GetChildren(wxDataViewItem(), roots);

	std::vector<CSearchFile *> parents;
	parents.reserve(roots.GetCount());
	for (size_t i = 0; i < roots.GetCount(); ++i) {
		parents.push_back(CSearchListModel::ToFile(roots[i]));
	}
	std::sort(parents.begin(), parents.end(), byDisplayOrder);

	// An expanded group's children occupy rows of their own, so they belong
	// here too: callers count rows against what is on screen (a page is
	// GetCountPerPage() rows, children included) and select ranges of them.
	// Leaving them out made a page overshoot and skipped every child inside
	// the range.
	ordered.clear();
	ordered.reserve(parents.size());
	for (CSearchFile *parent : parents) {
		ordered.push_back(parent);

		const wxDataViewItem item = CSearchListModel::ToItem(parent);
		if (!IsExpanded(item)) {
			continue;
		}
		wxDataViewItemArray kids;
		m_model->GetChildren(item, kids);
		std::vector<CSearchFile *> children;
		children.reserve(kids.GetCount());
		for (size_t i = 0; i < kids.GetCount(); ++i) {
			children.push_back(CSearchListModel::ToFile(kids[i]));
		}
		std::sort(children.begin(), children.end(), byDisplayOrder);
		ordered.insert(ordered.end(), children.begin(), children.end());
	}
}

void CSearchListCtrl::OnPopupDownload(wxCommandEvent &event)
{
	if (event.GetId() == MP_RESUME) {
		DownloadSelected();
	} else {
		DownloadSelected(event.GetId() - MP_ASSIGNCAT);
	}
}

void CSearchListCtrl::DownloadSelected(int category)
{
	FindWindowById(IDC_SDOWNLOAD)->Enable(false);

	if (category == -1) {
		category = 0;
		if (CastByID(IDC_EXTENDEDSEARCHCHECK, NULL, wxCheckBox)->GetValue()) {
			category = CastByID(ID_AUTOCATASSIGN, NULL, wxChoice)->GetSelection();
		}
	}

#ifndef CLIENT_GUI
	// Monolithic: Search_Add_Download runs synchronously on this thread, so
	// each selected file's Notify_DownloadCtrlAddFile -> AddFile fires a
	// per-item resort inline. Batch the whole selection into a single sort
	// + repaint (issue #615). The remote GUI's adds arrive later via the
	// download-queue poll, which already batches, so this is monolithic-only.
	CDownloadListCtrl *downloadlist = theApp->amuledlg->m_transferwnd->downloadlistctrl;
	downloadlist->BeginBatchUpdate();
#endif

	wxDataViewItemArray selections;
	GetSelections(selections);
	for (const wxDataViewItem &item : selections) {
		CSearchFile *file = CSearchListModel::ToFile(item);
		// Exempt from "Hide Known Files" before queueing, so the row
		// survives the status change this is about to cause. Results are
		// only grouped when their hashes match (CSearchList::AddResult), so
		// the one insert covers a group's variants along with its parent.
		m_userQueued.insert(file->GetFileHash());
		CoreNotify_Search_Add_Download(file, category);
	}

#ifndef CLIENT_GUI
	downloadlist->EndBatchUpdate();
#endif
	// List gets updated by notification when download is started
}

wxString CSearchListCtrl::DetermineStatusPrintable(CSearchFile *toshow)
{
	switch (toshow->GetDownloadStatus()) {
	case CSearchFile::DOWNLOADED:
		return _("Downloaded");
	case CSearchFile::QUEUED:
	case CSearchFile::QUEUEDCANCELED:
		return _("Queued");
	case CSearchFile::CANCELED:
		return _("Canceled");
	default:
		return _("New");
	}
}
// File_checked_for_headers
