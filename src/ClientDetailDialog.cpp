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

#include "ClientDetailDialog.h" // Interface declarations
#include "PartFile.h"           // Needed for CPartFile
#include "UploadQueue.h"        // Needed for CUploadQueue
#include "ServerList.h"         // Needed for CServerList
#include "amule.h"              // Needed for theApp
#include "Server.h"             // Needed for CServer
#include "muuli_wdr.h"          // Needed for ID_CLOSEWND
#include "Preferences.h"        // Needed for thePrefs

// CClientDetailDialog dialog

wxBEGIN_EVENT_TABLE(CClientDetailDialog, wxDialog)
	EVT_BUTTON(ID_CLOSEWND, CClientDetailDialog::OnBnClose)
wxEND_EVENT_TABLE()

ClientDetailInfo ClientDetailInfoFromClient(const CClientRef &client)
{
	CClientRef &c = const_cast<CClientRef &>(client);
	ClientDetailInfo info;
	info.userName = c.GetUserName();
	info.userHash = c.GetUserHash();
	info.softStr = c.GetSoftStr();
	info.osInfo = c.GetClientOSInfo();
	info.softVerStr = c.GetSoftVerStr();
	info.fullIp = c.GetFullIP();
	info.userPort = c.GetUserPort();
	info.obfuscationStatus = c.GetObfuscationStatus();
	info.uploadedTotal = c.GetUploadedTotal();
	info.downloadedTotal = c.GetDownloadedTotal();

	info.hasSession = true;
	info.userIdHybrid = c.GetUserIDHybrid();
	info.lowId = c.HasLowID();
	info.serverIp = c.GetServerIP();
	info.serverPort = c.GetServerPort();
	info.serverName = c.GetServerName();
	info.kadPort = c.GetKadPort();
	info.modCapabilities = c.GetModCapabilitiesText();
	info.uploadFile = c.GetUploadFile();
	info.transferredDown = c.GetTransferredDown();
	info.transferredUp = c.GetTransferredUp();
	info.kBpsDown = c.GetKBpsDown();
	info.uploadDatarate = c.GetUploadDatarate();
	info.scoreRatio = c.GetScoreRatio();
	info.secureIdentStatus = c.GetSecureIdentTextStatus();
	info.uploadState = c.GetUploadState();
	info.queueRank = c.GetUploadQueueWaitingPosition();
	info.score = c.GetScore();
	return info;
}

// Both constructors run the same setup; only where m_info came from differs.
void CClientDetailDialog::Build()
{
	wxSizer *content = clientDetails(this, true);
	// The Close button uses ID_CLOSEWND rather than wxID_CANCEL, so
	// wxDialog doesn't auto-bind Escape to it. Tell wxDialog to treat
	// ID_CLOSEWND as the escape target so pressing Escape dismisses
	// the dialog the same way clicking Close does.
	SetEscapeId(ID_CLOSEWND);
	OnInitDialog();
	content->SetSizeHints(this);
	content->Show(this, true);
}

CClientDetailDialog::CClientDetailDialog(wxWindow *parent, const CClientRef &client)
: wxDialog(parent, 9997, _("Client Details"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
{
	m_info = ClientDetailInfoFromClient(client);
	Build();
}

CClientDetailDialog::CClientDetailDialog(wxWindow *parent, const ClientDetailInfo &info)
: wxDialog(parent, 9997, _("Client Details"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
{
	m_info = info;
	Build();
}

CClientDetailDialog::~CClientDetailDialog() {}

void CClientDetailDialog::OnBnClose(wxCommandEvent &WXUNUSED(evt))
{
	EndModal(0);
}

bool CClientDetailDialog::OnInitDialog()
{
	// The dash the session fields fall back to when there is no live peer
	// behind this dialog. Not "0": a zero rate or a zero queue rank is a
	// measurement, and these are the absence of one.
	static const wxString kNoValue = "-";

	// Username and hash are reported independently. A credit record keeps a
	// hash long before it has a name, because the core only writes the name
	// out at disconnect, so the two are not known together and the list row
	// is already showing that hash beside this dialog.
	CastChild(ID_DNAME, wxStaticText)
		->SetLabel(m_info.userName.IsEmpty() ? _("Unknown") : m_info.userName);
	CastChild(ID_DHASH, wxStaticText)
		->SetLabel(m_info.userHash.IsEmpty() ? _("Unknown") : m_info.userHash.Encode());

	// Client Software
	if (!m_info.osInfo.IsEmpty()) {
		CastChild(ID_DSOFT, wxStaticText)->SetLabel(m_info.softStr + " (" + m_info.osInfo + ")");
	} else {
		CastChild(ID_DSOFT, wxStaticText)->SetLabel(m_info.softStr);
	}

	// Client Version
	CastChild(ID_DVERSION, wxStaticText)->SetLabel(m_info.softVerStr);

	// User ID
	CastChild(ID_DID, wxStaticText)
		->SetLabel(m_info.hasSession ? wxString(CFormat("%u (%s)") % m_info.userIdHybrid %
							(m_info.lowId ? _("LowID") : _("HighID")))
					     : kNoValue);

	// Client IP/Port
	CastChild(ID_DIP, wxStaticText)
		->SetLabel(m_info.fullIp.IsEmpty()
				   ? kNoValue
				   : wxString(CFormat("%s:%i") % m_info.fullIp % m_info.userPort));

	// Server IP/Port/Name
	if (m_info.serverIp) {
		wxString srvaddr = Uint32toStringIP(m_info.serverIp);
		CastChild(ID_DSIP, wxStaticText)->SetLabel(CFormat("%s:%i") % srvaddr % m_info.serverPort);
		CastChild(ID_DSNAME, wxStaticText)->SetLabel(m_info.serverName);
	} else {
		CastChild(ID_DSIP, wxStaticText)->SetLabel(_("Unknown"));
		CastChild(ID_DSNAME, wxStaticText)->SetLabel(_("Unknown"));
	}

	// Obfuscation
	wxString buffer;
	switch (m_info.obfuscationStatus) {
	case OBST_ENABLED:
		buffer = _("Enabled");
		break;
	case OBST_SUPPORTED:
		buffer = _("Supported");
		break;
	case OBST_NOT_SUPPORTED:
		buffer = _("Not supported");
		break;
	case OBST_DISABLED:
		buffer = _("Disabled");
		break;
	default:
		buffer = _("Unknown");
		break;
	}
	CastChild(IDT_OBFUSCATION, wxStaticText)->SetLabel(buffer);

	// Protocol extensions the peer claims -- not what this build can do with
	// them: aMule implements none of these yet, so the line reads as "this
	// peer would support X if we did".
	//
	// Label and value are both hidden when there is nothing to claim, rather
	// than shown with a placeholder. Almost no peer on the network sets any
	// of these bits, and a stored row has no hello to read at all, so the
	// alternative is a row that reads "None" or "-" for nearly every peer,
	// permanently -- a row that says nothing while taking up the space of one
	// that does. Hiding both controls leaves no gap: the sizer skips a hidden
	// pair, and Layout() below reflows what is left.
	const bool hasCapabilities = m_info.hasSession && !m_info.modCapabilities.IsEmpty();
	CastChild(IDT_MOD_CAPABILITIES_LABEL, wxStaticText)->Show(hasCapabilities);
	wxStaticText *capabilities = CastChild(IDT_MOD_CAPABILITIES, wxStaticText);
	capabilities->Show(hasCapabilities);
	if (hasCapabilities) {
		capabilities->SetLabel(m_info.modCapabilities);
	}

	// Kad
	if (!m_info.hasSession) {
		CastChild(IDT_KAD, wxStaticText)->SetLabel(kNoValue);
	} else if (m_info.kadPort) {
		CastChild(IDT_KAD, wxStaticText)->SetLabel(_("Connected"));
	} else {
		CastChild(IDT_KAD, wxStaticText)->SetLabel(_("Disconnected"));
	}

	// File Name
	if (m_info.uploadFile) {
		wxString filename = MakeStringEscaped(m_info.uploadFile->GetFileName().TruncatePath(60));
		CastChild(ID_DDOWNLOADING, wxStaticText)->SetLabel(filename);
	} else {
		CastChild(ID_DDOWNLOADING, wxStaticText)->SetLabel(kNoValue);
	}

	// Upload
	CastChild(ID_DDUP, wxStaticText)
		->SetLabel(m_info.hasSession ? CastItoXBytes(m_info.transferredDown) : kNoValue);

	// Download
	CastChild(ID_DDOWN, wxStaticText)
		->SetLabel(m_info.hasSession ? CastItoXBytes(m_info.transferredUp) : kNoValue);

	// Average Upload Rate
	CastChild(ID_DAVUR, wxStaticText)
		->SetLabel(
			m_info.hasSession ? wxString(CFormat(_("%.1f KiB/s")) % m_info.kBpsDown) : kNoValue);

	// Average Download Rate
	CastChild(ID_DAVDR, wxStaticText)
		->SetLabel(m_info.hasSession ? wxString(CFormat(_("%.1f KiB/s")) %
							((float)m_info.uploadDatarate / 1024.0f))
					     : kNoValue);

	// Lifetime credit, which a stored record knows as well as a live peer.
	// The control ids read backwards against their labels -- ID_DUPTOTAL sits
	// under "Downloaded (total):" -- so follow the labels, not the names.
	CastChild(ID_DUPTOTAL, wxStaticText)->SetLabel(CastItoXBytes(m_info.downloadedTotal));
	CastChild(ID_DDOWNTOTAL, wxStaticText)->SetLabel(CastItoXBytes(m_info.uploadedTotal));

	// DL/UP Modifier
	CastChild(ID_DRATIO, wxStaticText)
		->SetLabel(m_info.hasSession ? wxString(CFormat("%.1f") % m_info.scoreRatio) : kNoValue);

	// Secure Ident
	CastChild(IDC_CDIDENT, wxStaticText)
		->SetLabel(m_info.hasSession ? m_info.secureIdentStatus : kNoValue);

	// Queue Score
	if (m_info.hasSession && m_info.uploadState != US_NONE) {
		CastChild(ID_QUEUERANK, wxStaticText)->SetLabel(CFormat("%u") % m_info.queueRank);
		CastChild(ID_DSCORE, wxStaticText)->SetLabel(CFormat("%u") % m_info.score);
	} else {
		CastChild(ID_QUEUERANK, wxStaticText)->SetLabel(kNoValue);
		CastChild(ID_DSCORE, wxStaticText)->SetLabel(kNoValue);
	}
	Layout();

	return true;
}
// File_checked_for_headers
