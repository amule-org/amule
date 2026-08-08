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

#include "SearchListModel.h"

#include <algorithm>     // Needed for std::min, std::find
#include <unordered_map> // Needed for std::unordered_map (additions per parent)
#include <unordered_set> // Needed for std::unordered_set (live-result check)

#include <common/Format.h> // Needed for CFormat
#include <tags/FileTags.h> // Needed for FT_MEDIA_LENGTH / _BITRATE / _CODEC

#include "amule.h"          // Needed for theApp
#include "amuleDlg.h"       // Needed for CamuleDlg, Client_*_Smiley
#include "MuleColour.h"     // Needed for IsListBackgroundDark
#include "OtherFunctions.h" // Needed for CastItoXBytes, GetFiletypeByName, GetRateString, FormatMediaCodec
#include "SearchDlg.h"      // Needed for CSearchListCtrl
#include "SearchFile.h"     // Needed for CSearchFile
#include "SearchList.h"     // Needed for CSearchList, CSearchResultList
#include "SearchListCtrl.h"

namespace
{
/**
 * Whether arrivals may be reported to the control one at a time.
 *
 * Only on macOS, and only with no filter in force.
 *
 * The filter half is about correctness: m_filterKnown makes a row's
 * visibility a live function of its download status, so a value change can
 * require a row to appear or disappear, which only re-evaluating the tree
 * catches.
 *
 * The platform half is about wx. Its GTK backend keeps its own mirror of the
 * model's tree, and feeding this model's arrivals into it a notification at
 * a time corrupts GtkTreeView's red-black tree outright --
 *
 *   gtkrbtree.c:471:_gtk_rbtree_insert_after:
 *     assertion failed: (_gtk_rbtree_is_nil (tree->root))
 *
 * -- which aborts the application. That was first seen for ItemAdded() under
 * a newly formed group; confining the incremental path to top-level
 * additions and value changes did not avoid it, as the same abort came back
 * from the root batch. PR #796 had already found that neither ItemChanged()
 * nor a delete-and-re-add made GTK or MSW re-derive container-ness, and
 * settled on Cleared() for that reason; two aborts from two different
 * notifications say the constraint is broader than container-ness, so this
 * stops trying to find the subset GTK tolerates.
 *
 * Native NSOutlineView keeps no such structure -- it re-queries the model as
 * it draws, which is what masked the original #796 defect -- so it takes the
 * incremental path happily, and that is where the scroll position was being
 * lost on every burst of results.
 *
 * MSW is grouped with GTK deliberately: its generic implementation has tree
 * bookkeeping of its own, it was never the platform this was tested on, and
 * keeping today's behaviour there costs nothing but a repaint.
 */
bool IncrementalNotificationsUsable(const CSearchListCtrl *owner)
{
#ifdef __WXOSX__
	return !owner->HasActiveFilter();
#else
	(void)owner;
	return false;
#endif
}
} // namespace

std::set<CSearchListModel *> CSearchListModel::s_models;

CSearchListModel::CSearchListModel(CSearchListCtrl *owner)
: m_owner(owner)
{
	s_models.insert(this);
}

CSearchListModel::~CSearchListModel()
{
	s_models.erase(this);
}

void CSearchListModel::DropReferencesTo(CSearchFile *file)
{
	for (CSearchListModel *model : s_models) {
		model->m_pendingAdded.erase(
			std::remove(model->m_pendingAdded.begin(), model->m_pendingAdded.end(), file),
			model->m_pendingAdded.end());
		model->m_pendingChanged.erase(
			std::remove(model->m_pendingChanged.begin(), model->m_pendingChanged.end(), file),
			model->m_pendingChanged.end());
	}
}

void CSearchListModel::NotifyFileAdded(CSearchFile *file)
{
	if (file == nullptr || !IncrementalNotificationsUsable(m_owner)) {
		MarkDirty();
		return;
	}
	m_pendingAdded.push_back(file);
}

void CSearchListModel::NotifyFileUpdated(CSearchFile *file)
{
	if (file == nullptr || !IncrementalNotificationsUsable(m_owner)) {
		MarkDirty();
		return;
	}
	m_pendingChanged.push_back(file);
}

void CSearchListModel::NotifyFilterChanged()
{
	// User action: reset now rather than waiting for idle.
	DropPending();
	m_pendingReset = false;
	Cleared();
}

void CSearchListModel::DropPending()
{
	m_pendingAdded.clear();
	m_pendingChanged.clear();
}

bool CSearchListModel::FlushPending()
{
	if (m_pendingReset) {
		// Supersedes anything queued: Cleared() re-reads the whole tree.
		DropPending();
		m_pendingReset = false;
		Cleared();
		return true;
	}

	if (m_pendingAdded.empty() && m_pendingChanged.empty()) {
		return false;
	}

	// Nothing here can be dangling: DropReferencesTo() removes a result the
	// moment it is destroyed. Duplicates can be, and are: SetDownloadStatus()
	// notifies every child of the parent it updated, so one arrival into a
	// 50-variant group queues 51 entries and a busy idle window multiplies
	// that. The control is handed each row once.
	std::unordered_set<const CSearchFile *> seen;

	// Additions are reported per parent, since wx takes one parent for a
	// whole batch: top-level results under the root, grouped ones under the
	// result they joined.
	wxDataViewItemArray addedRoots;
	std::unordered_map<CSearchFile *, wxDataViewItemArray> addedChildren;
	for (CSearchFile *file : m_pendingAdded) {
		if (!seen.insert(file).second) {
			continue;
		}
		CSearchFile *parent = const_cast<CSearchFile *>(file->GetParent());
		if (parent == nullptr) {
			addedRoots.Add(ToItem(file));
		} else {
			addedChildren[parent].Add(ToItem(file));
		}
	}

	seen.clear();
	wxDataViewItemArray changed;
	for (CSearchFile *file : m_pendingChanged) {
		if (seen.insert(file).second) {
			changed.Add(ToItem(file));
		}
	}
	DropPending();

	// Incremental, so the control keeps its scroll position, its selection
	// and its expanded rows -- which Cleared() destroys, and which used to
	// go on every burst of arriving results.
	if (!addedRoots.IsEmpty()) {
		ItemsAdded(wxDataViewItem(), addedRoots);
	}
	for (const auto &entry : addedChildren) {
		ItemsAdded(ToItem(entry.first), entry.second);
	}
	if (!changed.IsEmpty()) {
		ItemsChanged(changed);
	}
	return false;
}

unsigned int CSearchListModel::GetColumnCount() const
{
	return COL_COUNT;
}

wxString CSearchListModel::GetColumnType(unsigned int col) const
{
	return (col == COL_RATING) ? wxString("wxDataViewIconText") : wxString("string");
}

void CSearchListModel::GetValue(wxVariant &variant, const wxDataViewItem &item, unsigned int col) const
{
	CSearchFile *file = ToFile(item);

	switch (col) {
	case COL_NAME:
		variant = file->GetFileName().GetPrintable();
		break;

	case COL_SIZE:
		variant = CastItoXBytes(file->GetFileSize());
		break;

	case COL_SOURCES: {
		wxString temp = CFormat("%d") % file->GetSourceCount();
		if (file->GetCompleteSourceCount()) {
			temp += CFormat(" (%d)") % file->GetCompleteSourceCount();
		}
		if (file->GetClientsCount()) {
			temp += CFormat(" [%d]") % file->GetClientsCount();
		}
#if defined(__DEBUG__) && !defined(CLIENT_GUI)
		if (file->GetKadPublishInfo() == 0) {
			temp += " | -";
		} else {
			temp += CFormat(" | N:%u, P:%u, T:%0.2f") %
				((file->GetKadPublishInfo() & 0xFF000000) >> 24) %
				((file->GetKadPublishInfo() & 0x00FF0000) >> 16) %
				((file->GetKadPublishInfo() & 0x0000FFFF) / 100.0);
		}
#endif
		variant = temp;
		break;
	}

	case COL_TYPE:
		variant = GetFiletypeByName(file->GetFileName());
		break;

	case COL_RATING: {
		wxString label;
		wxIcon icon;
		if (file->HasRating()) {
			label = GetRateString(file->UserRating());
			const int image = Client_InvalidRating_Smiley + file->UserRating() - 1;
			icon = theApp->amuledlg->m_imagelist.GetIcon(image);
		}
		variant << wxDataViewIconText(label, icon);
		break;
	}

	case COL_FILEID:
		variant = file->GetFileHash().Encode();
		break;

	case COL_STATUS:
		variant = CSearchListCtrl::DetermineStatusPrintable(file);
		break;

	case COL_LENGTH: {
		uint32 lenSec = file->GetIntTagValue(FT_MEDIA_LENGTH);
		variant = lenSec ? CastSecondsToHM(lenSec) : wxString();
		break;
	}

	case COL_BITRATE: {
		uint32 bitrate = file->GetIntTagValue(FT_MEDIA_BITRATE);
		variant = bitrate ? wxString(CFormat(wxT("%u kbps")) % bitrate) : wxString();
		break;
	}

	case COL_CODEC: {
		const wxString &codec = file->GetStrTagValue(FT_MEDIA_CODEC);
		variant = codec.IsEmpty() ? wxString() : FormatMediaCodec(codec);
		break;
	}

	case COL_DIRECTORY:
		variant = file->GetDirectory();
		break;

	default:
		break;
	}
}

bool CSearchListModel::SetValue(
	const wxVariant &WXUNUSED(variant), const wxDataViewItem &WXUNUSED(item), unsigned int WXUNUSED(col))
{
	// Every column is read-only (wxDATAVIEW_CELL_INERT); values only ever
	// change from the core side, via NotifyFileUpdated().
	return false;
}

bool CSearchListModel::GetAttr(
	const wxDataViewItem &item, unsigned int WXUNUSED(col), wxDataViewItemAttr &attr) const
{
	CSearchFile *file = ToFile(item);

	// Same theme-aware state palette as the old
	// CSearchListCtrl::UpdateItemColor -- see MuleColour.h's
	// IsListBackgroundDark() for why the widget's own background is used
	// rather than wxSystemSettings::GetAppearance().IsDark() alone.
	const bool isDark = IsListBackgroundDark(m_owner);
	wxColour colour = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);

	switch (file->GetDownloadStatus()) {
	case CSearchFile::DOWNLOADED:
		colour = isDark ? wxColour(80, 220, 80) : wxColour(0, 160, 0);
		break;
	case CSearchFile::QUEUED:
	case CSearchFile::QUEUEDCANCELED:
		colour = isDark ? wxColour(255, 100, 100) : wxColour(220, 0, 0);
		break;
	case CSearchFile::CANCELED:
		colour = isDark ? wxColour(255, 120, 200) : wxColour(180, 0, 180);
		break;
	default: {
		const int shift = std::min((int)file->GetSourceCount() * 5, 180);
		colour = isDark ? wxColour(255 - shift, 255 - shift, 255) : wxColour(0, 0, shift);
		break;
	}
	}

	attr.SetColour(colour);
	return true;
}

wxDataViewItem CSearchListModel::GetParent(const wxDataViewItem &item) const
{
	if (!item.IsOk()) {
		return wxDataViewItem();
	}
	CSearchFile *parent = ToFile(item)->GetParent();
	return parent ? ToItem(parent) : wxDataViewItem();
}

bool CSearchListModel::IsContainer(const wxDataViewItem &item) const
{
	if (!item.IsOk()) {
		return true; // invisible root
	}
	// Must agree with GetChildren(), which only yields children that pass
	// the filter: answering "has children" here for a group whose variants
	// are all filtered out draws an expander that opens onto nothing
	// (got3nks, PR #796 review).
	const CSearchFile *file = ToFile(item);
	const CSearchResultList &kids = file->GetChildren();
	for (const CSearchFile *kid : kids) {
		if (m_owner->PassesFilter(kid)) {
			return true;
		}
	}
	return false;
}

bool CSearchListModel::HasContainerColumns(const wxDataViewItem &WXUNUSED(item)) const
{
	return true;
}

unsigned int CSearchListModel::GetChildren(const wxDataViewItem &item, wxDataViewItemArray &children) const
{
	if (!item.IsOk()) {
		if (!m_owner->GetSearchId()) {
			return 0;
		}
		const CSearchResultList &results =
			theApp->searchlist->GetSearchResults(m_owner->GetSearchId());
		unsigned int count = 0;
		for (CSearchFile *file : results) {
			if (!file->GetParent() && m_owner->ShouldShow(file)) {
				children.Add(ToItem(file));
				++count;
			}
		}
		return count;
	}

	CSearchFile *parent = ToFile(item);
	const CSearchResultList &kids = parent->GetChildren();
	unsigned int count = 0;
	for (CSearchFile *kid : kids) {
		if (m_owner->PassesFilter(kid)) {
			children.Add(ToItem(kid));
			++count;
		}
	}
	return count;
}

int CSearchListModel::Compare(const wxDataViewItem &item1,
	const wxDataViewItem &item2,
	unsigned int WXUNUSED(column),
	bool WXUNUSED(ascending)) const
{
	// The requested (column, ascending) pair reflects only the primary
	// native header click; the full multi-column tie-break chain (set up
	// by earlier clicks on other columns) is tracked on the control itself
	// and applied here regardless of what native state triggered this
	// particular resort -- see CSearchListCtrl::CompareFiles.
	return m_owner->CompareFiles(ToFile(item1), ToFile(item2));
}
