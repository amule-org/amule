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

#include <wx/arrstr.h>
#include <wx/dc.h>
#include <wx/image.h>
#include <protocol/ed2k/ClientSoftware.h>
#include "Constants.h"
#include "OtherFunctions.h" // Needed for LabelWithColon
#include "amuleDlg.h"       // ClientSkinEnum

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

wxBitmap CClientImages::GetTintedBitmap(
	size_t index, const wxSize &logicalSize, double contentScale, const wxColour &tint) const
{
	if (!tint.IsOk()) {
		return GetBitmap(index, logicalSize, contentScale);
	}
	const auto key = std::make_tuple(index, logicalSize.x, logicalSize.y, contentScale, tint.GetRGB());
	const auto cached = m_tinted.find(key);
	if (cached != m_tinted.end()) {
		return cached->second;
	}
	wxBitmap bitmap = GetBitmap(index, logicalSize, contentScale);
	if (bitmap.IsOk()) {
		wxImage image = bitmap.ConvertToImage();
		image.SetRGB(wxRect(image.GetSize()), tint.Red(), tint.Green(), tint.Blue());
		bitmap = wxBitmap(image);
		bitmap.SetScaleFactor(contentScale);
	}
	return m_tinted.emplace(key, bitmap).first->second;
}

namespace
{
const int BADGE_POSITIONS = 3;

struct ClientNameLayout
{
	bool valid = false;
	wxSize imageSize;
	wxSize badgeSize;
	int clientX = 0;
	int badgeX = 0;
	int badgeStep = 0;
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
	layout.badgeSize = dc.FromDIP(wxSize(10, 10));
	const int slot = layout.imageSize.x + dc.FromDIP(2);
	const int badgeGap = dc.FromDIP(1);
	layout.badgeStep = layout.badgeSize.x + badgeGap;
	layout.clientX = cell.showState ? slot : 0;
	layout.badgeX = layout.clientX + slot;
	layout.flagX = layout.badgeX + BADGE_POSITIONS * layout.badgeStep - badgeGap + dc.FromDIP(2);
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
		case SO_EMULEPLUS: // An eMule fork with its own client ID.
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
			// cDonkey, Hydranode, Compatible, Unknown: no icon for those yet;
			// falls back to Client_Unknown.
			break;
		}
	}

	const int realY = point.y + imageYOffset;
	draw(clientImage, point.x, realY);

	const int badgeY = realY + (layout.imageSize.y - layout.badgeSize.y) / 2;
	const auto badge = [&](const wxBitmap &bitmap, int position) {
		if (bitmap.IsOk()) {
			dc->DrawBitmap(bitmap,
				rect.GetX() + layout.badgeX + position * layout.badgeStep,
				badgeY,
				true);
		}
	};
	if (cell.identified) {
		badge(images.GetBitmap(Client_SecIdent_Smiley, layout.badgeSize, contentScale), 0);
	} else if (cell.identFailed || cell.badGuy) {
		badge(images.GetBitmap(Client_BadGuy_Smiley, layout.badgeSize, contentScale), 0);
	}
	if (cell.obfuscation == OBST_ENABLED) {
		// In the name's colour, so it shows on every row background, the selection included.
		badge(images.GetTintedBitmap(Client_Encryption_Smiley,
			      layout.badgeSize,
			      contentScale,
			      dc->GetTextForeground()),
			1);
	}
	if (cell.highCredits) {
		badge(images.GetBitmap(Client_CreditsYellow_Smiley, layout.badgeSize, contentScale), 2);
	}

	if (flag.IsOk()) {
		const int flagY = point.y + (rect.GetHeight() - flag.GetLogicalSize().y) / 2 + 1;
		dc->DrawBitmap(flag, rect.GetX() + layout.flagX, flagY, true);
	}

	const int textOffset = ((rect.GetHeight() - dc->GetCharHeight()) / 2) + 1 /* Fixes rounding */;
	dc->DrawText(layout.text, rect.GetX() + layout.textX, rect.GetY() + textOffset);
}

wxString ClientNameCellToolTip(const ClientNameCell &cell)
{
	wxArrayString lines;
	// The Client Details dialog's words for the same states.
	if (cell.identified || cell.identFailed || cell.badGuy) {
		const wxString state = cell.identified    ? _("Verified - OK")
				       : cell.identFailed ? _("Failed")
							  : _("Bad Guy");
		lines.Add(LabelWithColon(_("Secure ident")) + " " + state);
	}
	if (cell.obfuscation == OBST_ENABLED) {
		lines.Add(LabelWithColon(_("Obfuscation")) + " " + _("Enabled"));
	}
	if (cell.highCredits) {
		/* TRANSLATORS: the upload credit a peer has earned by sending us data. */
		lines.Add(LabelWithColon(_("Credits")) + " " + _("Moves up faster in our upload queue"));
	}
	if (!cell.extProtocol) {
		lines.Add(_("Does not support the eMule extended protocol"));
	}
	return wxJoin(lines, '\n', 0);
}
// File_checked_for_headers
