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

#include "ClientNameCell.h" // Interface declarations

#include <wx/dc.h>
#include <wx/imaglist.h>

#include <protocol/ed2k/ClientSoftware.h>

#include "amule.h"    // Needed for theApp
#include "amuleDlg.h" // Needed for CamuleDlg::m_imagelist
#ifdef GEOIP_GUI
#include "CountryFlags.h"   // Needed for CCountryFlags (flag bitmaps)
#include "CountryDisplay.h" // Needed for GetDisplayCountryCode
#endif
// MUST match the build's client class: the reduced EC client for amulegui, the
// full one for monolithic. Including the wrong header gives this TU a different
// CUpDownClient layout than the rest of the (remote) GUI, so member reads land
// at the wrong offset.
#ifdef CLIENT_GUI
#include "UpDownClientEC.h"
#else
#include "updownclient.h"
#endif

ClientNameCell MakeClientNameCell(const CUpDownClient *client, bool a4af)
{
	ClientNameCell cell;
	if (client == nullptr) {
		return cell;
	}

	cell.name = client->GetUserName();
	cell.downloadState = static_cast<uint8>(client->GetDownloadState());
	cell.clientSoft = static_cast<uint8>(client->GetClientSoft());
	cell.obfuscation = static_cast<uint8>(client->GetObfuscationStatus());
	cell.a4af = a4af;
	cell.remoteQueueFull = client->IsRemoteQueueFull();
	cell.isFriend = client->IsFriend();
	cell.identified = client->IsIdentified();
	cell.badGuy = client->IsBadGuy();
	cell.extProtocol = client->ExtProtocolAvailable();
	cell.highCredits = client->GetScoreRatio() > 1;

#ifdef GEOIP_GUI
	// GetDisplayCountryCode() holds the shared gate (see CountryDisplay.h) so
	// every list that shows a flag stays in step.
	wxString code;
	if (GetDisplayCountryCode(client->IsCountryFromCore(),
		    client->GetCountryCode(),
		    client->GetFullIPNumeric(),
		    code)) {
		cell.countryCode = code;
	}
#endif

	return cell;
}

void DrawClientNameCell(const ClientNameCell &cell, const wxRect &rect, wxDC *dc)
{
	if (rect.GetWidth() <= 0 || rect.GetHeight() <= 0) {
		return;
	}

	wxDCClipper clipper(*dc, rect);

	wxImageList &imageList = theApp->amuledlg->m_imagelist;
	int imageXSize = 0;
	int imageYSize = 0;
	if (!imageList.GetSize(0, imageXSize, imageYSize)) {
		return;
	}
	imageXSize += 2; // Padding, matches DrawClientItem's iBitmapXSize.
	const int imageYOffset = ((rect.GetHeight() - imageYSize) / 2) + 1 /* Fixes rounding */;

	wxPoint point(rect.GetX(), rect.GetY());

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

	imageList.Draw(image, *dc, point.x, point.y + imageYOffset, wxIMAGELIST_DRAW_TRANSPARENT);
	point.x += imageXSize;

	uint8 clientImage = Client_Unknown;
	if (cell.isFriend) {
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
	imageList.Draw(clientImage, *dc, point.x, realY, wxIMAGELIST_DRAW_TRANSPARENT);

	if (cell.highCredits) {
		imageList.Draw(
			Client_CreditsYellow_Smiley, *dc, point.x, realY, wxIMAGELIST_DRAW_TRANSPARENT);
	} else if (!cell.extProtocol) {
		imageList.Draw(
			Client_ExtendedProtocol_Smiley, *dc, point.x, realY, wxIMAGELIST_DRAW_TRANSPARENT);
	}

	if (cell.identified) {
		imageList.Draw(Client_SecIdent_Smiley, *dc, point.x, realY, wxIMAGELIST_DRAW_TRANSPARENT);
	} else if (cell.badGuy) {
		imageList.Draw(Client_BadGuy_Smiley, *dc, point.x, realY, wxIMAGELIST_DRAW_TRANSPARENT);
	}

	if (cell.obfuscation == OBST_ENABLED) {
		imageList.Draw(Client_Encryption_Smiley, *dc, point.x, realY, wxIMAGELIST_DRAW_TRANSPARENT);
	}

	point.x += imageXSize;

#ifdef GEOIP_GUI
	if (!cell.countryCode.IsEmpty()) {
		const wxImage &flag = theApp->GetCountryFlags()->GetFlag(cell.countryCode);
		const int flagY = point.y + (rect.GetHeight() - flag.GetHeight()) / 2 + 1 /* floor() */;
		dc->DrawBitmap(flag, point.x, flagY, true);
		point.x += flag.GetWidth() + 2 /* Padding */;
	}
#endif // GEOIP_GUI

	const wxString userName = cell.name.IsEmpty() ? wxString("?") : cell.name;
	const int textOffset = ((rect.GetHeight() - dc->GetCharHeight()) / 2) + 1 /* Fixes rounding */;
	dc->DrawText(userName, point.x, rect.GetY() + textOffset);
}
// File_checked_for_headers
