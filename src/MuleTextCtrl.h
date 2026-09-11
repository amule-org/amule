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

#ifndef MULETEXTCTRL_H
#define MULETEXTCTRL_H

#include <wx/textctrl.h>
#include <wx/colour.h>

class wxCommandEvent;
class wxMouseEvent;
class wxFocusEvent;

/**
 * A slightly improved wxTextCtrl that offers the traditional text-control popup menu -- Cut, Copy,
 * Paste, Clear, Select All. Otherwise it acts exactly like an ordinary wxTextCtrl.
 */
class CMuleTextCtrl : public wxTextCtrl
{
public:
	/**
	 * Identical to the wxTextCtrl constructor.
	 */
	CMuleTextCtrl(wxWindow *parent,
		wxWindowID id,
		const wxString &value = "",
		const wxPoint &pos = wxDefaultPosition,
		const wxSize &size = wxDefaultSize,
		long style = 0,
		const wxValidator &validator = wxDefaultValidator,
		const wxString &name = wxTextCtrlNameStr);

	virtual ~CMuleTextCtrl() {};

	/**
	 * Enable a grey placeholder shown while the control is empty and unfocused, cleared
	 * automatically on focus or typing. Works on all platforms, unlike wxTextCtrl::SetHint(),
	 * which does nothing for multi-line controls under GTK/MSW.
	 */
	void SetPlaceholder(const wxString &hint);

	/**
	 * True while the placeholder text is being displayed, i.e. the user has not entered
	 * anything. Callers reading the value should treat this as an empty control.
	 */
	bool IsShowingPlaceholder() const { return m_showingPlaceholder; }

	/**
	 * Re-show the placeholder if the control is empty and unfocused. Call after
	 * programmatically clearing the value.
	 */
	void RefreshPlaceholder();

#ifdef __WXMAC__
	/**
	 * Hack to fix fonts getting reset when Clear() is called.
	 */
	virtual void Clear();
#endif

protected:
	/**
	 * Creates the popup menu. Using the RIGHT_DOWN event disables the second kind of selection
	 * wxTextCtrl supports, which is obscure enough that nobody is likely to miss it.
	 */
	void OnRightDown(wxMouseEvent &evt);

	/**
	 * Pastes text. Only needed because wxMenu disallows enabling and disabling of items using
	 * the predefined wxID_PASTE id; the other provided commands work as they are.
	 */
	void OnPaste(wxCommandEvent &evt);

	/**
	 * Selects all text.
	 */
	void OnSelAll(wxCommandEvent &evt);

	/**
	 * Clears the text.
	 */
	void OnClear(wxCommandEvent &evt);

	/**
	 * Placeholder focus handlers: clear the hint when the control gains focus, restore it on
	 * blur if the user left the control empty.
	 */
	void OnSetFocus(wxFocusEvent &evt);
	void OnKillFocus(wxFocusEvent &evt);

private:
	void ApplyPlaceholder();
	void RemovePlaceholder();

	wxString m_placeholder;
	wxColour m_normalColour;
	bool m_hasPlaceholder = false;
	bool m_showingPlaceholder = false;

	wxDECLARE_EVENT_TABLE();
};

#endif

// File_checked_for_headers
