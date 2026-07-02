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

#ifndef ABOUTDIALOG_H
#define ABOUTDIALOG_H

#include <wx/dialog.h>

#include "config.h" // for ENABLE_VERSION_CHECK

#include "VersionCheck.h"

class wxStaticText;
class wxButton;
class wxHyperlinkCtrl;

// The Help/About dialog. Shows the aMule version + credits with native
// clickable links (previously a plain wxMessageBox) and, when the version
// check is compiled in (ENABLE_VERSION_CHECK), a live "Check for updates"
// control backed by the shared CVersionCheck.
class CAboutDlg : public wxDialog
{
public:
	explicit CAboutDlg(wxWindow *parent);

#ifdef ENABLE_VERSION_CHECK

private:
	void OnCheckClicked(wxCommandEvent &evt);
	void OnCheckDone(wxCommandEvent &evt);

	CVersionCheck m_check;
	wxStaticText *m_status;
	wxHyperlinkCtrl *m_downloadLink;
	wxButton *m_checkButton;
#endif // ENABLE_VERSION_CHECK
};

#endif // ABOUTDIALOG_H
