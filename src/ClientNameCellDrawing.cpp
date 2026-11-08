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

#include "ClientNameCell.h"

#include <wx/dc.h>
#include <protocol/ed2k/ClientSoftware.h>
#include "Constants.h"
#include "amuleDlg.h" // ClientSkinEnum

wxBitmap CClientImages::GetBitmap(size_t index, const wxSize &logicalSize, double contentScale) const
{
	if (index >= m_bundles.size()) {
		return wxNullBitmap;
	}
	const auto key = std::make_tuple(index, logicalSize.x, logicalSize.y, contentScale);
	const auto cached = m_bitmaps.find(key);
	if (cached != m_bitmaps.end()) {
		return cached->second;
	}
	wxBitmap bitmap = m_bundles[index].GetBitmap(
		wxSize(wxRound(logicalSize.x * contentScale), wxRound(logicalSize.y * contentScale)));
	if (bitmap.IsOk()) {
		bitmap.SetScaleFactor(contentScale);
	}
	return m_bitmaps.emplace(key, bitmap).first->second;
}

namespace
{
struct ClientNameLayout
{
	bool valid = false;
	wxSize imageSize;
	int clientX = 0;
	int flagX = 0;
	int textX = 0;
	wxString text;
};

ClientNameLayout LayoutClientNameCell(
	const ClientNameCell &cell, wxDC &dc, const CClientImages &images, const wxBitmap &flag)
{
	ClientNameLayout layout;
	layout.valid = !images.GetBundles().empty();
	layout.imageSize = dc.FromDIP(wxSize(16, 16));
	const int slot = layout.imageSize.x + dc.FromDIP(2);
	layout.clientX = cell.showState ? slot : 0;
	layout.flagX = layout.clientX + slot;
	layout.textX = layout.flagX;
	if (flag.IsOk()) {
		layout.textX += flag.GetLogicalSize().x + dc.FromDIP(2);
	}
	layout.text = cell.name.IsEmpty() ? wxString("?") : cell.name;
	return layout;
}
} // namespace

int MeasureClientNameCell(
	const ClientNameCell &cell, wxDC &dc, const CClientImages &images, const wxBitmap &flag)
{
	const ClientNameLayout layout = LayoutClientNameCell(cell, dc, images, flag);
	return layout.valid ? layout.textX + dc.GetTextExtent(layout.text).x : 0;
}

void DrawClientNameCell(const ClientNameCell &cell,
	const wxRect &rect,
	wxDC &context,
	const CClientImages &images,
	const wxBitmap &flag)
{
	if (rect.GetWidth() <= 0 || rect.GetHeight() <= 0) {
		return;
	}

	wxDC *dc = &context;
	wxDCClipper clipper(*dc, rect);
	const ClientNameLayout layout = LayoutClientNameCell(cell, *dc, images, flag);
	if (!layout.valid) {
		return;
	}
#ifdef wxHAS_DPI_INDEPENDENT_PIXELS
	const double contentScale = dc->GetContentScaleFactor();
#else
	// Drawing coordinates are already physical pixels, even on a DC whose bitmap has a scale.
	const double contentScale = 1.0;
#endif
	const auto draw = [&](size_t index, int x, int y) {
		const wxBitmap bitmap = images.GetBitmap(index, layout.imageSize, contentScale);
		if (bitmap.IsOk()) {
			dc->DrawBitmap(bitmap, x, y, true);
		}
	};
	const int imageYOffset = ((rect.GetHeight() - layout.imageSize.y) / 2) + 1;
	wxPoint point(rect.GetX(), rect.GetY());

	if (cell.showState) {
		uint8 image = Client_Grey_Smiley;
		if (!cell.a4af) {
			switch (cell.downloadState) {
			case DS_CONNECTING:
			case DS_CONNECTED:
			case DS_WAITCALLBACK:
			case DS_TOOMANYCONNS:
				image = Client_Red_Smiley;
				break;
			case DS_ONQUEUE:
				image = cell.remoteQueueFull ? Client_Grey_Smiley : Client_Yellow_Smiley;
				break;
			case DS_DOWNLOADING:
			case DS_REQHASHSET:
				image = Client_Green_Smiley;
				break;
			case DS_NONEEDEDPARTS:
			case DS_LOWTOLOWIP:
				image = Client_Grey_Smiley; // Redundant
				break;
			default: // DS_NONE i.e.
				image = Client_White_Smiley;
			}
		} // else: default (Client_Grey_Smiley)

		draw(image, point.x, point.y + imageYOffset);
	}
	point.x = rect.GetX() + layout.clientX;

	uint8 clientImage = Client_Unknown;
	if (!cell.knownSoftware) {
		// Leave it unknown.
	} else if (cell.isFriend) {
		clientImage = Client_Friend_Smiley;
	} else {
		switch (cell.clientSoft) {
		case SO_AMULE:
			clientImage = Client_aMule_Smiley;
			break;
		case SO_MLDONKEY:
		case SO_NEW_MLDONKEY:
		case SO_NEW2_MLDONKEY:
			clientImage = Client_mlDonkey_Smiley;
			break;
		case SO_EDONKEY:
		case SO_EDONKEYHYBRID:
			clientImage = Client_eDonkeyHybrid_Smiley;
			break;
		case SO_EMULE:
			clientImage = Client_eMule_Smiley;
			break;
		case SO_LPHANT:
			clientImage = Client_lphant_Smiley;
			break;
		case SO_SHAREAZA:
		case SO_NEW_SHAREAZA:
		case SO_NEW2_SHAREAZA:
			clientImage = Client_Shareaza_Smiley;
			break;
		case SO_LXMULE:
			clientImage = Client_xMule_Smiley;
			break;
		default:
			// cDonkey, Compatible, Unknown: no icon for those yet;
			// falls back to Client_Unknown.
			break;
		}
	}

	const int realY = point.y + imageYOffset;
	draw(clientImage, point.x, realY);

	if (cell.highCredits) {
		draw(Client_CreditsYellow_Smiley, point.x, realY);
	} else if (!cell.extProtocol) {
		draw(Client_ExtendedProtocol_Smiley, point.x, realY);
	}

	if (cell.identified) {
		draw(Client_SecIdent_Smiley, point.x, realY);
	} else if (cell.identWarning) {
		draw(Client_BadGuy_Smiley, point.x, realY);
	}

	if (cell.obfuscation == OBST_ENABLED) {
		draw(Client_Encryption_Smiley, point.x, realY);
	}

	if (flag.IsOk()) {
		const int flagY = point.y + (rect.GetHeight() - flag.GetLogicalSize().y) / 2 + 1;
		dc->DrawBitmap(flag, rect.GetX() + layout.flagX, flagY, true);
	}

	const int textOffset = ((rect.GetHeight() - dc->GetCharHeight()) / 2) + 1 /* Fixes rounding */;
	dc->DrawText(layout.text, rect.GetX() + layout.textX, rect.GetY() + textOffset);
}
// File_checked_for_headers
