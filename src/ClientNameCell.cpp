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

#include <wx/arrstr.h>
#include <wx/dc.h>

#include "amule.h"          // Needed for theApp
#include "amuleDlg.h"       // Needed for CamuleDlg::m_clientImages
#include "Constants.h"      // Needed for OBST_ENABLED
#include "OtherFunctions.h" // Needed for LabelWithColon
#ifdef GEOIP_GUI
#include "CountryFlags.h"   // Needed for CCountryFlags (flag bitmaps)
#include "CountryDisplay.h" // Needed for GetDisplayCountryCode
#endif
// MUST match the build's client class: the reduced EC client for amulegui, the full one for
// monolithic. The wrong header gives this TU a different CUpDownClient layout than the rest of the
// GUI, so member reads land at the wrong offset.
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
	cell.identWarning = client->IsBadGuy() || client->SUIFailed();
	cell.extProtocol = client->ExtProtocolAvailable();
	cell.highCredits = client->GetCreditRatio() > 1;

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

wxString ClientNameCellToolTip(const ClientNameCell &cell)
{
	wxArrayString lines;
	if (cell.identified || cell.identWarning) {
		lines.Add(LabelWithColon(_("Secure ident")) + " " +
			  (cell.identified ? _("Verified - OK") : _("Failed")));
	}
	if (cell.obfuscation == OBST_ENABLED) {
		lines.Add(LabelWithColon(_("Obfuscation")) + " " + _("Enabled"));
	}
	if (cell.highCredits) {
		lines.Add(_("Credits: this peer moves up faster in our upload queue"));
	}
	if (!cell.extProtocol) {
		lines.Add(_("Does not support the eMule extended protocol"));
	}
	return wxJoin(lines, '\n', 0);
}

namespace
{
wxBitmap GetClientFlag(const ClientNameCell &cell, wxDC &dc)
{
#ifdef GEOIP_GUI
	if (!cell.countryCode.IsEmpty()) {
		return theApp->GetCountryFlags()->GetFlag(
			cell.countryCode, dc.FromDIP(wxSize(16, 12)), dc.GetContentScaleFactor());
	}
#else
	(void)cell;
	(void)dc;
#endif
	return wxBitmap();
}
} // namespace

int MeasureClientNameCell(const ClientNameCell &cell, wxDC &dc)
{
	return MeasureClientNameCell(cell, dc, theApp->amuledlg->m_clientImages, GetClientFlag(cell, dc));
}

void DrawClientNameCell(const ClientNameCell &cell, const wxRect &rect, wxDC *dc)
{
	if (rect.GetWidth() <= 0 || rect.GetHeight() <= 0) {
		return;
	}
	DrawClientNameCell(cell, rect, *dc, theApp->amuledlg->m_clientImages, GetClientFlag(cell, *dc));
}
// File_checked_for_headers
