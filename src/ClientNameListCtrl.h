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

#ifndef CLIENTNAMELISTCTRL_H
#define CLIENTNAMELISTCTRL_H

#include <optional>

#include "ClientNameCell.h" // Needed for ClientNameCell
#include "MuleVirtualDataViewCtrl.h"

/**
 * A list of peers with a Name column drawn by DrawClientNameCell(): the per-file client lists in
 * Downloads and Shared files, and both lists of the Clients page.
 *
 * The column's drawing, width fitting and tooltip live here once. A subclass appends the column
 * with AddNameColumn() and says how one of its rows becomes a ClientNameCell.
 */
class CClientNameListCtrl : public CMuleVirtualDataViewCtrl
{
public:
	CClientNameListCtrl(wxWindow *parent,
		wxWindowID winid,
		const wxPoint &pos,
		const wxSize &size,
		long style,
		const wxString &name = "clientnamelistctrl");

protected:
	//! The Name cell of the row holding `data`, or none when the row has gone.
	virtual std::optional<ClientNameCell> NameCellOf(wxUIntPtr data) const = 0;

	//! Appends the Name column.
	void AddNameColumn(
		const wxString &label, unsigned modelColumn, const wxString &key, int width, int flags);

	int GetCustomFitWidth(const wxDataViewItem &item, unsigned column, wxDC &dc) const override;
	wxString GetCellToolTip(const wxDataViewItem &item, unsigned column) const override;
	//! Fills the Name column. A subclass with other bar columns calls this for that column.
	void GetItemBarFill(wxUIntPtr data, unsigned column, CBarFillSpec &out) const override;

private:
	class NameRenderer;

	std::optional<ClientNameCell> NameCellAt(const wxDataViewItem &item, unsigned column) const;

	std::optional<unsigned> m_nameColumn;
};

#endif // CLIENTNAMELISTCTRL_H
