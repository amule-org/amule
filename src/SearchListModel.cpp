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

#include <algorithm> // Needed for std::min

#include <common/Format.h> // Needed for CFormat
#include <tags/FileTags.h> // Needed for FT_MEDIA_LENGTH / _BITRATE / _CODEC

#include "amule.h"          // Needed for theApp
#include "amuleDlg.h"       // Needed for CamuleDlg, Client_*_Smiley
#include "MuleListCtrl.h"   // Needed for IsListBackgroundDark
#include "OtherFunctions.h" // Needed for CastItoXBytes, GetFiletypeByName, GetRateString, FormatMediaCodec
#include "SearchDlg.h"      // Needed for CSearchListCtrl
#include "SearchFile.h"     // Needed for CSearchFile
#include "SearchList.h"     // Needed for CSearchList, CSearchResultList
#include "SearchListCtrl.h"

CSearchListModel::CSearchListModel(CSearchListCtrl *owner)
: m_owner(owner)
{
}

void CSearchListModel::NotifyFileAdded(CSearchFile *file)
{
	if (!m_owner->ShouldShow(file)) {
		return;
	}
	CSearchFile *parent = file->GetParent();
	ItemAdded(parent ? ToItem(parent) : wxDataViewItem(), ToItem(file));
}

void CSearchListModel::NotifyFileRemoved(CSearchFile *file)
{
	// Mirror NotifyFileAdded's gate: a file the control was never told about
	// (filtered out at add time) must not be deleted from it either -- that
	// gate is exactly ShouldShow(), the same test GetChildren() itself uses
	// to decide what's visible (root items via ShouldShow(); children via
	// PassesFilter(), which ShouldShow() reduces to for a childless file).
	if (!m_owner->ShouldShow(file)) {
		return;
	}
	CSearchFile *parent = file->GetParent();
	ItemDeleted(parent ? ToItem(parent) : wxDataViewItem(), ToItem(file));
}

void CSearchListModel::NotifyFileUpdated(CSearchFile *file)
{
	ItemChanged(ToItem(file));
}

void CSearchListModel::NotifyFilterChanged()
{
	// The set of visible root items (and which parents count as containers)
	// may have changed arbitrarily; a full reset is the only thing that's
	// guaranteed correct here, at the cost of resetting expand state -- the
	// same cost EnableFiltering()/SetFilter() already paid via
	// DeleteAllItems()-equivalent behaviour before this port.
	Cleared();
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
	// CSearchListCtrl::UpdateItemColor -- see MuleListCtrl.h's
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
	return ToFile(item)->HasChildren();
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
