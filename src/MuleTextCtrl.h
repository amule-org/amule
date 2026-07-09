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
 * This class is a slightly improved wxTextCtrl that supports the traditional
 * popup-menu usually provided by text-ctrls. It provides the following options:
 *  - Cut
 *  - Copy
 *  - Paste
 *  - Clear
 *  - Select All
 *
 * Other than that, it acts exactly like an ordinary wxTextCtrl.
 */
class CMuleTextCtrl : public wxTextCtrl
{
public:
	/**
	 * Constructor is identical to the wxTextCtrl one.
	 */
	CMuleTextCtrl(wxWindow *parent,
		wxWindowID id,
		const wxString &value = "",
		const wxPoint &pos = wxDefaultPosition,
		const wxSize &size = wxDefaultSize,
		long style = 0,
		const wxValidator &validator = wxDefaultValidator,
		const wxString &name = wxTextCtrlNameStr);

	/**
	 * Destructor, which currently does nothing.
	 */
	virtual ~CMuleTextCtrl() {};

	/**
	 * Enable a grey placeholder shown while the control is empty and
	 * unfocused, cleared automatically on focus/typing. Works on all
	 * platforms (unlike wxTextCtrl::SetHint(), which does nothing for
	 * multi-line controls under GTK/MSW).
	 */
	void SetPlaceholder(const wxString &hint);

	/**
	 * True while the placeholder text is being displayed, i.e. the user
	 * has not entered anything. Callers reading the value should treat
	 * this as an empty control.
	 */
	bool IsShowingPlaceholder() const { return m_showingPlaceholder; }

	/**
	 * Re-show the placeholder if the control is empty and unfocused.
	 * Call this after programmatically clearing the value.
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
	 * This function takes care of creating the popup-menu.
	 *
	 * Please note that by using the RIGHT_DOWN event, I'm disabling the second
	 * type of selection that the wxTextCtrl supports. However, I frankly only
	 * noticed that second selection type while implementing this, so I doubt
	 * that anyone will be missing it ...
	 */
	void OnRightDown(wxMouseEvent &evt);

	/**
	 * This function takes care of pasting text.
	 *
	 * Please note that it is only needed because wxMenu disallows enabling and
	 * disabling of items that use the predefined wxID_PASTE id. This is the
	 * only one of the already provided commands we need to override, since the
	 * others already work just fine.
	 */
	void OnPaste(wxCommandEvent &evt);

	/**
	 * This functions takes care of selecting all text.
	 */
	void OnSelAll(wxCommandEvent &evt);

	/**
	 * This functions takes care of clearing the text.
	 */
	void OnClear(wxCommandEvent &evt);

	/**
	 * Placeholder focus handlers: clear the hint when the control gains
	 * focus, restore it on blur if the user left the control empty.
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
