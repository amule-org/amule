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

#include "ClientListSplitter.h"

#include <wx/artprov.h>
#include <wx/bmpbuttn.h>
#include <wx/splitter.h>
#include <wx/sizer.h>
#include <wx/utils.h>

#include "GenericClientListCtrl.h"

namespace
{
constexpr int s_splitterMin = 90;
}

CClientListSplitter::CClientListSplitter(wxSplitterWindow &splitter,
	CGenericClientListCtrl &clients,
	wxBitmapButton &button,
	wxSizer &header,
	int &savedPosition)
: m_splitter(splitter)
, m_clients(clients)
, m_button(button)
, m_header(header)
, m_savedPosition(savedPosition)
{
}

void CClientListSplitter::Prepare()
{
	int height = m_splitter.GetSize().GetHeight();
	int header_height = m_header.GetSize().GetHeight();

	if (m_savedPosition) {
		// Some sanity checking
		if (m_savedPosition < s_splitterMin) {
			m_savedPosition = s_splitterMin;
		} else if (m_savedPosition > height - header_height * 2) {
			m_savedPosition = height - header_height * 2;
		}
		m_splitter.SetSashPosition(m_savedPosition);
		m_savedPosition = 0;
	}

	if (!m_clients.GetShowing()) {
		// Close the list and update its button after restoring the saved position.
		m_clients.SetShowing(true); // so it will be toggled to false
		Toggle();
	}
}

void CClientListSplitter::Toggle()
{
	if (!m_clients.GetShowing()) {
		m_splitter.SetSashPosition(m_savedPosition);
		m_savedPosition = 0;

		m_clients.SetShowing(true);
	} else {
		m_clients.SetShowing(false);

		m_savedPosition = m_splitter.GetSashPosition();

		// Add the height of the listctrl to the top-window
		int height = m_clients.GetSize().GetHeight() + m_splitter.GetWindow1()->GetSize().GetHeight();

		m_splitter.SetSashPosition(height);
	}
	const wxBitmapBundle art = wxArtProvider::GetBitmapBundle(
		m_clients.GetShowing() ? "amule:arrows_down" : "amule:arrows_up");
	m_button.SetBitmapLabel(art);
	m_button.SetBitmapFocus(art);
	m_button.SetBitmapPressed(art);
	m_button.SetBitmapCurrent(art);
}

void CClientListSplitter::OnSashPositionChanging(wxSplitterEvent &evt)
{
	if (evt.GetSashPosition() < s_splitterMin) {
		evt.SetSashPosition(s_splitterMin);
	} else {
		int height = m_splitter.GetSize().GetHeight();
		int header_height = m_header.GetSize().GetHeight();
		int mousey = wxGetMousePosition().y - m_splitter.GetScreenRect().GetTop();

		if (!m_clients.GetShowing()) {
			// lower window hidden
			if (height - mousey < header_height * 2) {
				// no moving down if already hidden
				evt.Veto();
			} else {
				// show it
				// Restore at the mouse position when dragging a collapsed list open.
				m_savedPosition = mousey;
				Toggle();
			}
		} else {
			// lower window showing
			if (height - mousey < header_height * 2) {
				// hide it
				Toggle();
			} else {
				// Normal resize. If several events queue up, setting the sash to
				// the current mouse position speeds things up and makes sash moving
				// smoother.
				evt.SetSashPosition(mousey);
			}
		}
	}
}

// File_checked_for_headers
