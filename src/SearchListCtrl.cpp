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

wxBEGIN_EVENT_TABLE(CSearchListCtrl, wxDataViewCtrl)
	EVT_DATAVIEW_ITEM_CONTEXT_MENU(wxID_ANY, CSearchListCtrl::OnRightClick)
	EVT_DATAVIEW_COLUMN_HEADER_CLICK(wxID_ANY, CSearchListCtrl::OnColumnHeaderClick)
	EVT_DATAVIEW_COLUMN_HEADER_RIGHT_CLICK(wxID_ANY, CSearchListCtrl::OnColumnHeaderRightClick)
	EVT_DATAVIEW_ITEM_ACTIVATED(wxID_ANY, CSearchListCtrl::OnItemActivated)
	EVT_DATAVIEW_SELECTION_CHANGED(wxID_ANY, CSearchListCtrl::OnSelectionChanged)
	EVT_IDLE(CSearchListCtrl::OnIdle)
	EVT_CHAR(CSearchListCtrl::OnChar)
	EVT_KEY_DOWN(CSearchListCtrl::OnKeyDown)

	EVT_MENU(MP_GETED2KLINK, CSearchListCtrl::OnPopupGetUrl)
	EVT_MENU(MP_RAZORSTATS, CSearchListCtrl::OnRazorStatsCheck)
	EVT_MENU(MP_SEARCHRELATED, CSearchListCtrl::OnRelatedSearch)
	EVT_MENU(MP_GETCOMMENTS, CSearchListCtrl::OnGetComments)
	EVT_MENU(MP_RESUME, CSearchListCtrl::OnPopupDownload)
	EVT_MENU_RANGE(MP_ASSIGNCAT, MP_ASSIGNCAT + 99, CSearchListCtrl::OnPopupDownload)
	EVT_MENU_RANGE(MP_LISTCOL_1, MP_LISTCOL_15, CSearchListCtrl::OnColumnMenuSelected)
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
// wxDataViewCtrl defaults to single selection, while the wxListCtrl this
// replaces was multi-select unless given wxLC_SINGLE_SEL -- without
// wxDV_MULTIPLE every GetSelections() caller below (download, copy links,
// category assign, ...) can only ever act on one result.
: wxDataViewCtrl(parent, winid, pos, size, wxDV_ROW_LINES | wxDV_MULTIPLE, wxDefaultValidator, name)
, m_nResultsID(0)
, m_browseEcid(0)
, m_browseStatus(0)
, m_widthAdapter(this)
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

	AppendTextColumn(_("File Name"),
		CSearchListModel::COL_NAME,
		wxDATAVIEW_CELL_INERT,
		500,
		wxALIGN_LEFT,
		wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
	AppendTextColumn(_("Size"),
		CSearchListModel::COL_SIZE,
		wxDATAVIEW_CELL_INERT,
		100,
		wxALIGN_LEFT,
		wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
	AppendTextColumn(_("Sources"),
		CSearchListModel::COL_SOURCES,
		wxDATAVIEW_CELL_INERT,
		50,
		wxALIGN_LEFT,
		wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
	AppendTextColumn(_("Type"),
		CSearchListModel::COL_TYPE,
		wxDATAVIEW_CELL_INERT,
		65,
		wxALIGN_LEFT,
		wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
	// Rating: smiley icon + text label in one cell.
	AppendIconTextColumn(_("Rating"),
		CSearchListModel::COL_RATING,
		wxDATAVIEW_CELL_INERT,
		120,
		wxALIGN_LEFT,
		wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
	AppendTextColumn(_("FileID"),
		CSearchListModel::COL_FILEID,
		wxDATAVIEW_CELL_INERT,
		280,
		wxALIGN_LEFT,
		wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
	AppendTextColumn(_("Status"),
		CSearchListModel::COL_STATUS,
		wxDATAVIEW_CELL_INERT,
		100,
		wxALIGN_LEFT,
		wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
	// Media tag columns: ed2k/Kad publishers (eMule, eMule AI, aMule) can
	// advertise per-file media metadata in FT_MEDIA_LENGTH / _BITRATE /
	// _CODEC. Cells stay empty for non-media results.
	AppendTextColumn(_("Length"),
		CSearchListModel::COL_LENGTH,
		wxDATAVIEW_CELL_INERT,
		80,
		wxALIGN_LEFT,
		wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
	AppendTextColumn(_("Bitrate"),
		CSearchListModel::COL_BITRATE,
		wxDATAVIEW_CELL_INERT,
		80,
		wxALIGN_LEFT,
		wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
	AppendTextColumn(_("Codec"),
		CSearchListModel::COL_CODEC,
		wxDATAVIEW_CELL_INERT,
		80,
		wxALIGN_LEFT,
		wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
	// Directories is almost always empty (only populated when the result
	// came from a "view shared files" request, rare in practice), so put
	// it at the end with the other usually-empty columns.
	AppendTextColumn(
		_("Directories"), // I would have preferred "Directory" but this is already translated
		CSearchListModel::COL_DIRECTORY,
		wxDATAVIEW_CELL_INERT,
		280,
		wxALIGN_LEFT,
		wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);

	m_columnStore.RegisterColumn(CSearchListModel::COL_NAME, 500, "N");
	m_columnStore.RegisterColumn(CSearchListModel::COL_SIZE, 100, "Z");
	m_columnStore.RegisterColumn(CSearchListModel::COL_SOURCES, 50, "u");
	m_columnStore.RegisterColumn(CSearchListModel::COL_TYPE, 65, "Y");
	m_columnStore.RegisterColumn(CSearchListModel::COL_RATING, 120, "R");
	m_columnStore.RegisterColumn(CSearchListModel::COL_FILEID, 280, "I");
	m_columnStore.RegisterColumn(CSearchListModel::COL_STATUS, 100, "S");
	m_columnStore.RegisterColumn(CSearchListModel::COL_LENGTH, 80, "L");
	m_columnStore.RegisterColumn(CSearchListModel::COL_BITRATE, 80, "B");
	m_columnStore.RegisterColumn(CSearchListModel::COL_CODEC, 80, "C");
	m_columnStore.RegisterColumn(CSearchListModel::COL_DIRECTORY, 280, "D");

	// Sized before LoadColumnSettings(), which restores hidden columns through
	// the width adapter and therefore writes into this.
	m_columnHidden.assign(GetColumnCount(), false);

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

	for (int i = 0; i < GetColumnCount(); ++i) {
		m_lastKnownWidths.push_back(GetColumn(i)->GetWidth());
	}

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

void CSearchListCtrl::LoadColumnSettings()
{
	if (!m_columnStore.HasTableName()) {
		return;
	}

	CListColumnStore::CSortingList decoded;
	m_columnStore.LoadSettings(m_widthAdapter, "N,Z,u,Y,I,S", decoded);

	// Restored widths can leave the default expander column hidden, which
	// would strand the group triangles on a column nobody can see.
	UpdateExpanderColumn();

	// LoadSettings() returns the orders primary-LAST: CMuleListCtrl applied
	// them by calling SetSorting() on each in turn, and each call pushes to
	// the front, so the last one processed ends up primary. ApplySorting()
	// has the same push-to-front semantics, so replaying them in order
	// reproduces that -- taking front() as the primary instead (as this
	// used to) picks the least significant entry (got3nks, PR #796 review).
	m_sort_orders.clear();
	for (const CListColumnStore::CColPair &pair : decoded) {
		ApplySorting(pair.first, pair.second);
	}
	if (m_sort_orders.empty()) {
		ApplySorting(CSearchListModel::COL_NAME, 0);
	}
}

void CSearchListCtrl::SaveColumnSettings()
{
	if (!m_columnStore.HasTableName()) {
		return;
	}
	m_columnStore.SaveSettings(m_widthAdapter, m_sort_orders);
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
			result = file->GetDownloadStatus() == CSearchFile::NEW;
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
	m_model->NotifyFilterChanged(); // full reset: new search-id, entirely different result set
}

void CSearchListCtrl::SetFilter(const wxString &regExp, bool invert, bool filterKnown)
{
	m_filterText = regExp.IsEmpty() ? wxString(".*") : regExp;
	m_filter.Compile(m_filterText, wxRE_DEFAULT | wxRE_ICASE);
	m_filterKnown = filterKnown;
	m_invert = invert;

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
	for (CSearchFile *file : results) {
		if (!file->GetParent() && !ShouldShow(file)) {
			++hidden;
		}
		// Children are only ever "hidden" for filtering purposes via their
		// own IsFiltered() test -- a filtered-in child under a filtered-out
		// parent still counts as visible (the parent surfaces as a
		// container for it), so it is not counted as hidden here either.
		if (file->GetParent() && !IsFiltered(file)) {
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

int CSearchListCtrl::CompareByColumn(
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
	for (const CColPair &entry : m_sort_orders) {
		const unsigned column = entry.first;
		const unsigned order = entry.second;
		const int modifier = (order & SORT_DES) ? -1 : 1;
		const bool alt = (order & SORT_ALT) != 0;

		int result = CompareByColumn(f1, f2, column, alt, modifier);
		if (result != 0) {
			return result;
		}
	}
	return 0;
}

bool CSearchListCtrl::AltSortAllowed(unsigned column) const
{
	return column == CSearchListModel::COL_SOURCES;
}

void CSearchListCtrl::ApplySorting(unsigned column, unsigned order)
{
	CSortingList::iterator it = m_sort_orders.begin();
	for (; it != m_sort_orders.end(); ++it) {
		if (it->first == column) {
			m_sort_orders.erase(it);
			break;
		}
	}
	m_sort_orders.emplace_front(column, order);

	// Unmark the previous sort column (only one wxDataViewColumn can be the
	// active sort key at a time; SetSortOrder() below moves the mark).
	for (int i = 0; i < GetColumnCount(); ++i) {
		if ((unsigned)i != column && GetColumn(i)->IsSortKey()) {
			GetColumn(i)->UnsetAsSortKey();
		}
	}
	GetColumn(column)->SetSortOrder(!(order & SORT_DES));
	GetModel()->Resort();

	SyncOtherLists(this);
}

void CSearchListCtrl::OnColumnHeaderClick(wxDataViewEvent &event)
{
	wxDataViewColumn *col = event.GetDataViewColumn();
	if (!col) {
		event.Skip();
		return;
	}
	const unsigned column = static_cast<unsigned>(col->GetModelColumn());

	// Mirrors CMuleListCtrl::OnColumnLClick's cycle: same column clicked
	// again flips ascending<->descending, and once descending, a further
	// click on an alt-eligible column flips to ascending with the alt
	// criterion toggled instead of clearing the sort.
	unsigned sort_order = 0;
	if (!m_sort_orders.empty() && m_sort_orders.front().first == column) {
		sort_order = m_sort_orders.front().second;
		if (sort_order & SORT_DES) {
			if (AltSortAllowed(column)) {
				sort_order = (~sort_order) & SORT_ALT;
			} else {
				sort_order = 0;
			}
		} else {
			sort_order = SORT_DES | (sort_order & SORT_ALT);
		}
	} else {
		for (CSortingList::const_iterator it = m_sort_orders.begin(); it != m_sort_orders.end();
			++it) {
			if (it->first == column) {
				sort_order = it->second;
				break;
			}
		}
	}

	ApplySorting(column, sort_order);
}

void CSearchListCtrl::SyncLists(CSearchListCtrl *src, CSearchListCtrl *dst)
{
	wxCHECK_RET(src && dst, "NULL argument in SyncLists");

	for (int i = 0; i < src->GetColumnCount(); ++i) {
		// Hidden state has to travel with the width: copying width alone
		// would leave the other tabs showing a column this one has hidden.
		const bool hidden = src->IsColumnHidden(i);
		if (dst->IsColumnHidden(i) != hidden) {
			dst->SetColumnHidden(i, hidden, src->GetColumn(i)->GetWidth());
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
			for (int i = 0; i < dst->GetColumnCount(); ++i) {
				if ((unsigned)i != primary.first && dst->GetColumn(i)->IsSortKey()) {
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

void CSearchListCtrl::OnIdle(wxIdleEvent &event)
{
	event.Skip();

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

	// No portable wxDataViewCtrl "column resized" event exists to hook
	// directly (unlike wxListCtrl's EVT_LIST_COL_END_DRAG), so a drag-resize
	// is detected here by simply comparing against the last-seen widths.
	bool changed = false;
	for (int i = 0; i < GetColumnCount() && i < (int)m_lastKnownWidths.size(); ++i) {
		const int width = GetColumn(i)->GetWidth();
		if (width != m_lastKnownWidths[i]) {
			m_lastKnownWidths[i] = width;
			changed = true;
		}
	}
	if (changed) {
		SyncOtherLists(this);
	}
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

void CSearchListCtrl::OnColumnHeaderRightClick(wxDataViewEvent &event)
{
	// Show/hide menu, as CMuleListCtrl::OnColumnRClick offered on every
	// other list. A column is "shown" when it is wider than COL_SIZE_MIN,
	// which is also how the state reaches the config: hiding writes a
	// zero-ish width that CListColumnStore persists like any other.
	wxMenu menu;
	const unsigned columns = std::min<unsigned>(GetColumnCount(), MP_LISTCOL_15 - MP_LISTCOL_1 + 1);
	for (unsigned i = 0; i < columns; ++i) {
		const wxDataViewColumn *col = GetColumn(i);
		menu.AppendCheckItem(static_cast<int>(i) + MP_LISTCOL_1, col->GetTitle());
		menu.Check(static_cast<int>(i) + MP_LISTCOL_1, !IsColumnHidden(static_cast<int>(i)));
	}

	PopupMenu(&menu);
	event.Skip();
}

bool CSearchListCtrl::IsColumnHidden(int col) const
{
	return (col >= 0) && (static_cast<size_t>(col) < m_columnHidden.size()) && m_columnHidden[col];
}

void CSearchListCtrl::SetColumnHidden(int col, bool hidden, int width)
{
	if (col < 0 || static_cast<unsigned>(col) >= GetColumnCount()) {
		return;
	}
	if (static_cast<size_t>(col) >= m_columnHidden.size()) {
		m_columnHidden.resize(GetColumnCount(), false);
	}

	m_columnHidden[col] = hidden;
	wxDataViewColumn *column = GetColumn(static_cast<unsigned>(col));
	column->SetHidden(hidden);
	if (!hidden && width > COL_SIZE_MIN) {
		column->SetWidth(width);
	}
}

void CSearchListCtrl::UpdateExpanderColumn()
{
	// The expander belongs on the leftmost visible column: hiding the column
	// that currently owns it would take the group triangles with it and
	// leave children unreachable, and re-showing a column to its left has
	// to take them back rather than stranding them mid-row.
	for (unsigned i = 0; i < GetColumnCount(); ++i) {
		if (!IsColumnHidden(static_cast<int>(i))) {
			wxDataViewColumn *column = GetColumn(i);
			if (GetExpanderColumn() != column) {
				SetExpanderColumn(column);
			}
			return;
		}
	}
}

void CSearchListCtrl::OnColumnMenuSelected(wxCommandEvent &evt)
{
	const int col = evt.GetId() - MP_LISTCOL_1;
	if (col < 0 || static_cast<unsigned>(col) >= GetColumnCount()) {
		return;
	}

	if (!IsColumnHidden(col)) {
		// Remember the width so re-showing restores what the user had,
		// exactly as CMuleListCtrl::OnMenuSelected did.
		m_columnStore.SetCachedWidth(col, GetColumn(static_cast<unsigned>(col))->GetWidth());
		SetColumnHidden(col, true, 0);
	} else {
		const int cached = m_columnStore.GetCachedWidth(col);
		SetColumnHidden(col, false, cached > 0 ? cached : m_columnStore.GetColumnDefaultWidth(col));
	}
	UpdateExpanderColumn();
	SyncOtherLists(this);
	SaveColumnSettings();
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

namespace
{
//! How long a pause resets the accumulated type-ahead string, in ms.
const uint64 kTypeAheadResetMs = 1500;
} // namespace

void CSearchListCtrl::BuildDisplayOrder(std::vector<CSearchFile *> &ordered) const
{
	// The model yields top-level rows in arrival order, so they are put
	// through this list's own comparator -- the one CSearchListModel::
	// Compare() uses -- to match what is actually on screen under the
	// current sort. GetItemByRow()/GetRowByItem() would be the direct
	// route but exist only in wx's generic implementation, not on GTK or
	// macOS.
	wxDataViewItemArray roots;
	m_model->GetChildren(wxDataViewItem(), roots);

	ordered.clear();
	ordered.reserve(roots.GetCount());
	for (size_t i = 0; i < roots.GetCount(); ++i) {
		ordered.push_back(CSearchListModel::ToFile(roots[i]));
	}
	std::sort(ordered.begin(), ordered.end(), [this](const CSearchFile *f1, const CSearchFile *f2) {
		return CompareFiles(f1, f2) < 0;
	});
}

#ifdef __WXOSX__
void CSearchListCtrl::PageExtendSelection(PageMotion motion)
{
	std::vector<CSearchFile *> ordered;
	BuildDisplayOrder(ordered);
	if (ordered.empty()) {
		return;
	}

	const wxDataViewItem currentItem = GetCurrentItem();
	CSearchFile *currentFile = currentItem.IsOk() ? CSearchListModel::ToFile(currentItem) : nullptr;
	const auto found = std::find(ordered.begin(), ordered.end(), currentFile);
	const bool forward = (motion == PageMotion::PageDown) || (motion == PageMotion::End);
	const int current = (found == ordered.end())
				    ? (forward ? -1 : static_cast<int>(ordered.size()))
				    : static_cast<int>(std::distance(ordered.begin(), found));

	const int last = static_cast<int>(ordered.size()) - 1;

	int target = 0;
	switch (motion) {
	case PageMotion::Home:
		target = 0;
		break;
	case PageMotion::End:
		target = last;
		break;
	default: {
		// GetCountPerPage() is implemented by the native macOS backend;
		// GetItemRect() is not a substitute, since it returns an empty rect
		// for rows that aren't currently on screen and the resulting height
		// of zero collapses a page to a single row.
		int rows = GetCountPerPage();
		if (rows <= 0) {
			rows = 10;
		} else {
			// Leave one row of overlap, as the other ports scroll.
			rows = std::max(1, rows - 1);
		}
		const int delta = (motion == PageMotion::PageDown) ? rows : -rows;
		target = std::min(last, std::max(0, current + delta));
		break;
	}
	}

	// Grow the existing selection to cover everything between where the
	// cursor was and where it lands, so repeated presses keep extending.
	wxDataViewItemArray selection;
	GetSelections(selection);
	const int from = std::min(current < 0 ? target : current, target);
	const int to = std::max(current > last ? target : current, target);
	for (int i = std::max(0, from); i <= std::min(last, to); ++i) {
		const wxDataViewItem item = CSearchListModel::ToItem(ordered[i]);
		if (selection.Index(item) == wxNOT_FOUND) {
			selection.Add(item);
		}
	}

	const wxDataViewItem targetItem = CSearchListModel::ToItem(ordered[target]);
	SetSelections(selection);
	SetCurrentItem(targetItem);
	EnsureVisible(targetItem);
}
#endif // __WXOSX__

void CSearchListCtrl::OnKeyDown(wxKeyEvent &evt)
{
#ifdef __WXOSX__
	// NSOutlineView moves the view without touching the selection, so the
	// shifted navigation keys -- which extend the selection on GTK and MSW
	// -- do nothing at all here. The unshifted forms are deliberately left
	// to the platform, which scrolls without moving the selection by
	// convention.
	if (evt.ShiftDown()) {
		switch (evt.GetKeyCode()) {
		case WXK_PAGEUP:
			PageExtendSelection(PageMotion::PageUp);
			return;
		case WXK_PAGEDOWN:
			PageExtendSelection(PageMotion::PageDown);
			return;
		case WXK_HOME:
			PageExtendSelection(PageMotion::Home);
			return;
		case WXK_END:
			PageExtendSelection(PageMotion::End);
			return;
		default:
			break;
		}
	}
#endif
	evt.Skip();
}

void CSearchListCtrl::OnChar(wxKeyEvent &evt)
{
	int key = evt.GetKeyCode();
	if (key == 0) {
		// GetKeyCode() returns 0 for characters it can't map; the unicode
		// key is the fallback (see CMuleListCtrl::OnChar for the history).
		// GetUnicodeKey() returns wxChar -- a signed char in wx's UTF-8
		// build but wchar_t in the wide build, so an unsigned-char cast
		// would truncate the wide case and the widening is left as-is.
		// NOLINTNEXTLINE(bugprone-signed-char-misuse)
		key = evt.GetUnicodeKey();
	} else if (key >= WXK_START) {
		// Arrows, page up/down, home/end: the backend's own cursor handling
		// owns these, and on macOS page keys deliberately scroll without
		// moving the selection (platform convention).
		evt.Skip();
		return;
	}

	if (evt.AltDown() || evt.ControlDown() || evt.MetaDown()) {
		// Cmd/Ctrl+A: only wxGTK's backend selects all by itself, so do it
		// here for all three rather than leaving macOS and MSW without it
		// (CMuleListCtrl::OnChar implemented it explicitly for the same
		// reason). A control-modified 'a' arrives as SOH on most ports,
		// but not universally, so accept the letter too.
		const int plain = wxTolower(evt.GetKeyCode());
		if (evt.CmdDown() && (evt.GetKeyCode() == 0x01 || plain == 'a')) {
			SelectAll();
			return;
		}
		// Every other shortcut belongs to the backend.
		evt.Skip();
		return;
	}

	const uint64 now = GetTickCount64();
	if (m_ttsTime + kTypeAheadResetMs < now) {
		m_ttsText.Clear();
	}
	m_ttsTime = now;
	m_ttsText.Append(wxTolower(static_cast<wxChar>(key)));

	// Match against the top-level rows in displayed order. The model yields
	// them in arrival order, so they are sorted through the list's own
	// comparator -- the same one CSearchListModel::Compare() uses -- rather
	// than a second ordering that could disagree with what is on screen.
	// (GetItemByRow()/GetRowByItem() would be the direct route but exist
	// only in wx's generic implementation, not on GTK or macOS.)
	std::vector<CSearchFile *> ordered;
	BuildDisplayOrder(ordered);
	if (ordered.empty()) {
		return;
	}

	// A fresh single keystroke starts one past the current match so that
	// tapping the same letter cycles; further keystrokes refine in place.
	size_t start = 0;
	const auto current = std::find(ordered.begin(), ordered.end(), m_ttsItem);
	if (current != ordered.end()) {
		start = static_cast<size_t>(std::distance(ordered.begin(), current));
		if (m_ttsText.length() == 1) {
			++start;
		}
	}

	const size_t count = ordered.size();
	for (size_t i = 0; i < count; ++i) {
		CSearchFile *file = ordered[(start + i) % count];
		if (file->GetFileName().GetPrintable().Lower().StartsWith(m_ttsText)) {
			const wxDataViewItem item = CSearchListModel::ToItem(file);
			m_ttsItem = file;
			UnselectAll();
			Select(item);
			SetCurrentItem(item);
			EnsureVisible(item);
			return;
		}
	}
	// No match: the accumulated text is kept (so a typo can be corrected by
	// continuing to type) but the selection stays where it is.
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
