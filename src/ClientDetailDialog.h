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

#ifndef CLIENTDETAILDIALOG_H
#define CLIENTDETAILDIALOG_H

#include <wx/dialog.h> // Needed for wxDialog

#include "ClientRef.h" // Needed for CClientRef
#include "MD4Hash.h"   // Needed for CMD4Hash
#include "Types.h"     // Needed for uint8/uint16/uint32/uint64

class CKnownFile;

/**
 * Everything CClientDetailDialog renders, separated from where it came from.
 *
 * A connected peer fills this from its live CUpDownClient. A row in the Known
 * list fills the identity half from the stored record and leaves `hasSession`
 * false, because the fields under it are session state that only a live
 * connection has -- rendering them as zeroes would read as real measurements.
 */
struct ClientDetailInfo
{
	// Identity and lifetime credit. A stored row knows these as well as a live
	// peer does; the credit totals are the point of keeping the row at all.
	wxString userName;
	CMD4Hash userHash;
	wxString softStr;
	wxString osInfo;
	wxString softVerStr;
	wxString fullIp;
	uint16 userPort = 0;
	uint8 obfuscationStatus = 0;
	uint64 uploadedTotal = 0;
	uint64 downloadedTotal = 0;

	//! False when there is no live client behind this. Everything below is
	//! session state and is shown as "-" instead.
	bool hasSession = false;
	uint32 userIdHybrid = 0;
	bool lowId = false;
	uint32 serverIp = 0;
	uint16 serverPort = 0;
	wxString serverName;
	uint16 kadPort = 0;
	//! Protocol extensions the peer claimed in its hello; a stored row has no
	//! hello to read. Empty when it claimed none, which is the common case
	//! and hides the row rather than filling it with a placeholder.
	wxString modCapabilities;
	const CKnownFile *uploadFile = nullptr;
	uint64 transferredDown = 0;
	uint64 transferredUp = 0;
	float kBpsDown = 0.0f;
	uint32 uploadDatarate = 0;
	double scoreRatio = 0.0;
	wxString secureIdentStatus;
	uint8 uploadState = 0;
	uint16 queueRank = 0;
	uint32 score = 0;
};

//! Snapshot a live peer. The session half is filled.
ClientDetailInfo ClientDetailInfoFromClient(const CClientRef &client);

/**
 * The ClientDetailDialog class is responsible for showing the info about a client.
 *
 * It shows all releavant data about the client: ip, port, hash, name, client
 * type and version, uploading/downloading data, credits, server... etc
 *
 * It's  wxDialog, modal, with return value always '0'.
 *
 */

class CClientDetailDialog : public wxDialog
{
public:
	/**
	 * Constructor for a live peer.
	 *
	 * @param parent The window that created the dialog.
	 * @param client The client whose details we're showing.
	 */
	CClientDetailDialog(wxWindow *parent, const CClientRef &client);

	/**
	 * Constructor for a peer we hold a record of but are not talking to.
	 *
	 * Renders the identity and credit half and shows the session fields as
	 * "-". Takes no client and opens no connection: showing details is a
	 * read-only act.
	 *
	 * @param parent The window that created the dialog.
	 * @param info The snapshot to render.
	 */
	CClientDetailDialog(wxWindow *parent, const ClientDetailInfo &info);

	/**
	 * Destructor.
	 *
	 * Does nothing currently.
	 */
	virtual ~CClientDetailDialog();

protected:
	/**
	 * Creates all the data objects in the dialog, filling them accordingly.
	 *
	 * Called when the dialog object is created.
	 */
	virtual bool OnInitDialog();

	//! Shared tail of both constructors.
	void Build();

	/**
	 * Ends the dialog, calling EndModal with return value 0
	 *
	 * @param evt The close event, unused right now
	 */
	void OnBnClose(wxCommandEvent &evt);

	wxDECLARE_EVENT_TABLE();

private:
	//! The client whose data is drawn
	ClientDetailInfo m_info;
};
#endif // CLIENTDETAILDIALOG_H
// File_checked_for_headers
