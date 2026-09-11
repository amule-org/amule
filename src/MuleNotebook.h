//
// This file is part of the aMule Project.
//
// Copyright (c) 2004-2011 Angel Vidal ( kry@amule.org )
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

#ifndef MULENOTEBOOK_H
#define MULENOTEBOOK_H

#include <wx/notebook.h>

wxDECLARE_EVENT(wxEVT_COMMAND_MULENOTEBOOK_PAGE_CLOSING, wxEvent);
wxDECLARE_EVENT(wxEVT_COMMAND_MULENOTEBOOK_ALL_PAGES_CLOSED, wxEvent);

#define EVT_MULENOTEBOOK_PAGE_CLOSING(id, fn) \
	wx__DECLARE_EVT1(wxEVT_COMMAND_MULENOTEBOOK_PAGE_CLOSING, \
		id, \
		wxEVENT_HANDLER_CAST(wxNotebookEventFunction, fn))
#define EVT_MULENOTEBOOK_ALL_PAGES_CLOSED(id, fn) \
	wx__DECLARE_EVT1(wxEVT_COMMAND_MULENOTEBOOK_ALL_PAGES_CLOSED, \
		id, \
		wxEVENT_HANDLER_CAST(wxNotebookEventFunction, fn))

class wxWindow;

/**
 * A wxNotebook with a few extra features: images on the tabs for closing pages, a popup menu for
 * closing one or more pages, and events triggered when pages are closed.
 */
class CMuleNotebook : public wxNotebook
{
public:
	/**
	 * @see wxNotebook::wxNotebook
	 */
	CMuleNotebook(wxWindow *parent,
		wxWindowID id,
		const wxPoint &pos = wxDefaultPosition,
		const wxSize &size = wxDefaultSize,
		long style = 0,
		const wxString &name = "notebook");

	virtual ~CMuleNotebook();

	/**
	 * Deletes page @a nPage and triggers an event.
	 */
	virtual bool DeletePage(int nPage);

	/**
	 * Deletes every page, triggering an event for each.
	 */
	virtual bool DeleteAllPages();

	/**
	 * Enables or disables the display of a popup menu.
	 */
	void EnablePopup(bool enable);

	/**
	 * Sets an external widget to handle the popup event, or NULL to disable. With one set, a
	 * right click sends a right-click event to that widget so it can create a popup menu; the
	 * coordinates are fixed to fit it, so no mapping is needed.
	 */
	void SetPopupHandler(wxWindow *widget);

protected:
	/**
	 * Left or middle mouse button press or release, for closing pages.
	 */
	void OnMouseButton(wxMouseEvent &event);

	/**
	 * Mouse motion, for highlighting the 'x'.
	 */
	void OnMouseMotion(wxMouseEvent &event);

	/**
	 * Right clicks, which display the popup menu.
	 */
	void OnRMButton(wxMouseEvent &event);

	/**
	 * The Close item on the popup menu.
	 */
	void OnPopupClose(wxCommandEvent &evt);

	/**
	 * The CloseAll item on the popup menu.
	 */
	void OnPopupCloseAll(wxCommandEvent &evt);

	/**
	 * The CloseOthers item on the popup menu.
	 */
	void OnPopupCloseOthers(wxCommandEvent &evt);

	//! Keeps track of the popup-menu being enabled or not.
	bool m_popup_enable;

	//! The pointer to the widget which would receive right-click events or NULL.
	wxWindow *m_popup_widget;

	wxDECLARE_EVENT_TABLE();
};

#ifdef __WINDOWS__
#define MULE_NOTEBOOK_TAB_HEIGHT 26
#else
#define MULE_NOTEBOOK_TAB_HEIGHT 40
#endif

#endif
// File_checked_for_headers
