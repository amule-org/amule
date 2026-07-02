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

#include "VersionCheck.h"

class wxTextCtrl;
class wxTextUrlEvent;
class wxStaticText;
class wxButton;
class wxHyperlinkCtrl;

// The Help/About dialog. Shows the aMule version + credits in a selectable,
// URL-clickable text area (previously a plain wxMessageBox) and adds a live
// "Check for updates" control backed by the shared CVersionCheck, so both the
// monolithic GUI and amulegui can tell the user whether a newer release is
// available and offer a clickable download link.
class CAboutDlg : public wxDialog
{
public:
	explicit CAboutDlg(wxWindow *parent);

private:
	void OnCheckClicked(wxCommandEvent &evt);
	void OnCheckDone(wxCommandEvent &evt);
	void OnTextUrl(wxTextUrlEvent &evt);

	CVersionCheck m_check;
	wxTextCtrl *m_aboutText;
	wxStaticText *m_status;
	wxHyperlinkCtrl *m_downloadLink;
	wxButton *m_checkButton;
};

#endif // ABOUTDIALOG_H
