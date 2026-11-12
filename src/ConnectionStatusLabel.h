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

#ifndef CONNECTION_STATUS_LABEL_H
#define CONNECTION_STATUS_LABEL_H

#include <algorithm>

#include <wx/stattext.h>

// Server names are supplied by the network. Their length must not determine
// how much of the status strip remains available for the latest log entry.
class ConnectionStatusLabel : public wxStaticText
{
public:
	ConnectionStatusLabel(wxWindow *parent, wxWindowID id, const wxString &text)
	: wxStaticText(
		  parent, id, text, wxDefaultPosition, wxDefaultSize, wxST_NO_AUTORESIZE | wxST_ELLIPSIZE_END)
	{
		// The base constructor measures its text before our sizing hooks are active.
		InvalidateBestSize();
		SetToolTip(RemoveMnemonics(text));
	}

	void SetLabel(const wxString &text) override
	{
		wxStaticText::SetLabel(text);
		// NO_AUTORESIZE can leave the previous text's measured size cached.
		InvalidateBestSize();
		// The label text escapes '&'; a tooltip shows it literally.
		SetToolTip(RemoveMnemonics(text));
	}

protected:
	wxSize DoGetBestSize() const override { return LimitWidth(wxStaticText::DoGetBestSize()); }

	wxSize DoGetBestClientSize() const override
	{
		// wxMSW supplies a client size, which takes precedence over DoGetBestSize().
		return LimitWidth(wxStaticText::DoGetBestClientSize());
	}

private:
	wxSize LimitWidth(wxSize best) const
	{
		// Preserve wxDefaultSize when a port does not implement the client-size hook.
		best.x = std::min(best.x, FromDIP(300));
		return best;
	}
};

#endif // CONNECTION_STATUS_LABEL_H
