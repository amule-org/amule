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

#include "SharedFilesReloadProgress.h"

#include <wx/progdlg.h>
#include <wx/window.h>

#include "SharedFileList.h"
#include "amule.h" // theApp
#include "OtherFunctions.h"

wxString SharedFilesScannedMessage(std::size_t filesScanned)
{
	return CFormat(_("Reloading shared files: %u files scanned")) % static_cast<unsigned>(filesScanned);
}

void ReloadSharedFilesWithProgress(wxWindow *parent)
{
#ifndef CLIENT_GUI
	const wxString body = SharedFilesScannedMessage(0);
	wxProgressDialog progress(_("Reloading shared files..."),
		body,
		/*maximum=*/100,
		parent,
		wxPD_APP_MODAL | wxPD_AUTO_HIDE);
	theApp->sharedfiles->Reload([&progress](size_t filesScanned) -> bool {
		progress.Pulse(SharedFilesScannedMessage(filesScanned));
		// Always continue: see the no-cancel note in the header.
		return true;
	});
	progress.Update(100);
#else
	// amulegui: the walk runs on amuled. CSharedFilesRem::Reload only posts
	// EC_OP_SHAREDFILES_RELOAD and returns, so a dialog would just flash.
	(void)parent;
	theApp->sharedfiles->Reload();
#endif
}

void ReloadSharedFilesIfPending(wxWindow *parent)
{
	if (theApp->sharedfiles && theApp->sharedfiles->IsReloadPending()) {
		ReloadSharedFilesWithProgress(parent);
	}
}
// File_checked_for_headers
