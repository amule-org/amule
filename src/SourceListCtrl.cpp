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
#include "SourceListCtrl.h"

#include <common/Format.h> // Needed for CFormat

#include "ClientRef.h" // Needed for CClientRef
#include "KnownFile.h"
#include "MuleBarRenderer.h" // Needed for CMuleBarRenderer, CBarFillSpec
#include "PartFile.h"        // Needed for CPartFile
#include "Preferences.h"     // Needed for thePrefs::ShowProgBar()

namespace
{
static CGenericClientListCtrlColumn s_sources_column_info[] = {
	{ ColumnUserName, wxTRANSLATE("User Name"), 260 },
	{ ColumnUserDownloaded, wxTRANSLATE("Downloaded"), 65 },
	{ ColumnUserSpeedDown, wxTRANSLATE("Speed"), 65 },
	{ ColumnUserUploaded, wxTRANSLATE("Uploaded"), 65 },
	{ ColumnUserProgress, wxTRANSLATE("Part Status"), 170 },
	{ ColumnUserVersion, wxTRANSLATE("Version"), 50 },
	{ ColumnUserQueueRankRemote, wxTRANSLATE("Download Status"), 55 },
	{ ColumnUserOrigin, wxTRANSLATE("Origin"), 110 },
	{ ColumnUserFileNameDownload, wxTRANSLATE("Local File Name"), 200 },
	{ ColumnUserFileNameDownloadRemote, wxTRANSLATE("Remote File Name"), 200 },
	{ ColumnUserSharedFiles, wxTRANSLATE("Shares File List"), 100 }
};

/**
 * Renders ColumnUserProgress: a client's per-part chunk bar (5 states: no
 * part / have-complete / downloading / next-requested / have-but-need), or,
 * for an A4AF row, a bordered "A4AF: <filename>" text badge instead of a bar
 * -- a real per-row renderer branch (not a bar overlay), replacing
 * DrawSourceStatusBar's A4AF case (GenericClientListCtrl.cpp, pre-port)
 * exactly, including its own themed border (own SetPen, not the base's black
 * one -- the base's Render() is not called for this branch at all).
 */
class CSourceBarRenderer : public CMuleBarRenderer
{
public:
	bool Render(wxRect cell, wxDC *dc, int state) override
	{
		// Gates the whole cell -- bar and A4AF badge alike -- exactly as the
		// pre-port ColumnUserProgress case did.
		if (!thePrefs::ShowProgBar()) {
			return true;
		}
		ClientCtrlItem_Struct *item =
			reinterpret_cast<ClientCtrlItem_Struct *>(GetSpec().GetIdentity());
		if (!item) {
			return true;
		}
		if (item->GetType() == A4AF_SOURCE) {
			DrawA4AFBadge(cell, dc, item);
			return true;
		}
		return CMuleBarRenderer::Render(cell, dc, state);
	}

private:
	static void DrawA4AFBadge(wxRect cell, wxDC *dc, ClientCtrlItem_Struct *item)
	{
		const int iWidth = cell.GetWidth() - 2;
		const int iHeight = cell.GetHeight() - 2;
		if (iWidth <= 0 || iHeight <= 0) {
			return;
		}
		wxDCClipper clipper(*dc, cell.GetX(), cell.GetY() + 1, iWidth, iHeight);

		CPartFile *p = item->GetSource().GetRequestFile();
		const wxString a4af = p ? p->GetFileName().GetPrintable() : wxString("?");
		const wxString buffer = CFormat("%s: %s") % _("A4AF") % a4af;

		const int mid_x = (2 * cell.GetX() + cell.GetWidth()) >> 1;
		const int mid_y = (2 * cell.GetY() + cell.GetHeight()) >> 1;
		wxCoord txtwidth;
		wxCoord txtheight;
		dc->GetTextExtent(buffer, &txtwidth, &txtheight);

		// Theme-aware text + border colour (was *wxBLACK / *wxBLACK_PEN,
		// invisible on dark themes -- the badge sits on the row stripe, not
		// on a known light background).
		const wxColour badgeColour = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
		dc->SetTextForeground(badgeColour);
		dc->DrawText(
			buffer, wxMax(cell.GetX() + 2, mid_x - (txtwidth >> 1)), mid_y - (txtheight >> 1));

		dc->SetPen(wxPen(badgeColour));
		dc->SetBrush(*wxTRANSPARENT_BRUSH);
		dc->DrawRectangle(cell.GetX(), cell.GetY() + 1, iWidth, iHeight);
	}
};
} // namespace

CSourceListCtrl::CSourceListCtrl(wxWindow *parent,
	wxWindowID winid,
	const wxPoint &pos,
	const wxSize &size,
	long style,
	const wxString &name)
: CGenericClientListCtrl("Sources", parent, winid, pos, size, style, name)
{
	m_columndata.n_columns = sizeof(s_sources_column_info) / sizeof(CGenericClientListCtrlColumn);
	m_columndata.columns = s_sources_column_info;

	InitColumnData();
}

CSourceListCtrl::~CSourceListCtrl() {}

CMuleBarRenderer *CSourceListCtrl::CreateProgressBarRenderer() const
{
	return new CSourceBarRenderer();
}

void CSourceListCtrl::SetShowSources(CKnownFile *f, bool b) const
{
	f->SetShowSources(b);
}

// File_checked_for_headers
