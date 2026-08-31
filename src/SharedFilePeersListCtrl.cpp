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
#include "SharedFilePeersListCtrl.h"
#include "KnownFile.h" // Do_not_auto_remove

namespace
{
CGenericClientListCtrlColumn s_sources_column_info[] = { { ColumnUserName, wxTRANSLATE("User Name"), 260 },
	{ ColumnUserDownloaded, wxTRANSLATE("Downloaded"), 65 },
	{ ColumnUserSpeedDown, wxTRANSLATE("Download Speed"), 65 },
	{ ColumnUserUploaded, wxTRANSLATE("Uploaded"), 65 },
	{ ColumnUserSpeedUp, wxTRANSLATE("Upload Speed"), 65 },
	{ ColumnUserAvailable, wxTRANSLATE("Parts on Peer"), 170 },
	{ ColumnUserVersion, wxTRANSLATE("Version"), 50 },
	{ ColumnUserQueueRankLocal, wxTRANSLATE("Upload Status"), 70 },
	{ ColumnUserQueueRankRemote, wxTRANSLATE("Download Status"), 70 },
	{ ColumnUserOrigin, wxTRANSLATE("Origin"), 110 },
	{ ColumnUserFileNameUpload, wxTRANSLATE("Local File Name"), 200 },
	{ ColumnUserSharedFiles, wxTRANSLATE("Shares File List"), 100 } };
} // namespace

CSharedFilePeersListCtrl::CSharedFilePeersListCtrl(wxWindow *parent,
	wxWindowID winid,
	const wxPoint &pos,
	const wxSize &size,
	long style,
	const wxString &name)
: CGenericClientListCtrl("Peers", parent, winid, pos, size, style, name)
{
	m_columndata.n_columns = sizeof(s_sources_column_info) / sizeof(CGenericClientListCtrlColumn);
	m_columndata.columns = s_sources_column_info;

	InitColumnData();
}

CSharedFilePeersListCtrl::~CSharedFilePeersListCtrl() {}

void CSharedFilePeersListCtrl::SetShowSources(CKnownFile *f, bool b) const
{
	f->SetShowPeers(b);
}

// File_checked_for_headers
