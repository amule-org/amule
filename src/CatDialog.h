//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2002-2011 quekky
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

#ifndef CATDIALOG_H
#define CATDIALOG_H

#include <wx/dialog.h> // Needed for wxDialog
#include "Types.h"     // Needed for uint32
#include "OtherStructs.h"
#include "MuleColour.h"

class wxStaticBitmap;
class wxBitmap;

/**
 * Displays an existing or new category so the user can add or change it.
 *
 * Self-contained: it does not rely on the categories staying the same while it is visible, though
 * it overwrites any change made to the selected category meanwhile, and re-adds the selected
 * category if it was deleted.
 *
 * It does rely on Transferwnd keeping its own category list up to date.
 */
class CCatDialog : public wxDialog
{
public:
	/**
	 * Constructor.
	 *
	 * @param parent The parent of the new dialog.
	 * @param catindex A valid index selects that category; less than zero creates a new one.
	 */
	CCatDialog(wxWindow *parent, bool allowbrowse, int catindex = -1);

	~CCatDialog();

private:
	/**
	 * Helper for the colour preview: a single-colour 16x16 image built from the m_colour
	 * member.
	 */
	wxBitmap MakeBitmap();

	//! Variable used to store the user-selected color.
	CMuleColour m_colour;

	//! Pointer to category to be edited or NULL if we are adding a new category.
	Category_Struct *m_category;

	/**
	 * Event handler: select the incoming dir.
	 */
	void OnBnClickedBrowse(wxCommandEvent &evt);

	/**
	 * Event handler: save the changes.
	 */
	void OnBnClickedOk(wxCommandEvent &evt);

	/**
	 * Event handler: select the category colour.
	 */
	void OnBnClickColor(wxCommandEvent &evt);

	wxDECLARE_EVENT_TABLE();
};

#endif
// File_checked_for_headers
