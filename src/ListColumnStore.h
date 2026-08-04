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

#ifndef LISTCOLUMNSTORE_H
#define LISTCOLUMNSTORE_H

#include <wx/string.h>

#include <list>
#include <utility>
#include <vector>

/**
 * The subset of a list widget's column API that column persistence needs.
 * CMuleListCtrl already implements this exact signature set by inheriting
 * wxGenericListCtrl, so it satisfies this interface for free (see its
 * class declaration). A future wxDataViewCtrl-backed list only needs to
 * forward these three calls to its own columns
 * (GetColumnCount()/GetColumn(i)->GetWidth()/SetWidth()) to reuse
 * CListColumnStore unchanged.
 */
class IColumnWidthProvider
{
public:
	virtual ~IColumnWidthProvider() = default;
	virtual int GetColumnCount() const = 0;
	virtual int GetColumnWidth(int col) const = 0;
	virtual bool SetColumnWidth(int col, int width) = 0;
};

/**
 * Owns a list control's column-persistence state (the stable, translation-
 * independent name/default-width per column, the last-known width cache,
 * and (de)serialization to/from wxConfig) independently of which widget
 * actually renders the columns.
 *
 * Extracted from CMuleListCtrl (see #675/#180 phase 2 discussion): the
 * persisted format and config keys ("/eMule/TableOrdering<name>",
 * "/eMule/TableWidths<name>") are unchanged, so existing user settings
 * keep working across this refactor. Sort-order *mutation* deliberately
 * stays out of this class -- applying a loaded sort order can have
 * widget-specific side effects (e.g. CMuleListCtrl::SetSorting() also
 * triggers a re-sort and validates against AltSortAllowed()), so
 * LoadSettings() only decodes the stored order and hands it back to the
 * caller to apply through whatever mechanism its own widget needs.
 */
class CListColumnStore
{
public:
	//! A column index paired with its CMuleListCtrl::MLOrder flags.
	typedef std::pair<unsigned, unsigned> CColPair;
	typedef std::list<CColPair> CSortingList;

	/**
	 * Sets the persistence key prefix. Mirrors
	 * CMuleListCtrl::SetTableName(): an empty name means "don't persist".
	 */
	void SetTableName(const wxString &name) { m_name = name; }
	bool HasTableName() const { return !m_name.IsEmpty(); }

	/**
	 * Registers a column's persistence name and default width at the
	 * given index. Called from the list's InsertColumn()-equivalent, the
	 * same point CMuleListCtrl::InsertColumn() currently updates
	 * m_column_names from.
	 */
	//! In debug builds, also asserts the name is free of ':'/',' (the
	//! serialization format's own delimiters) and not already registered --
	//! this store is the only thing that can check uniqueness now, so the
	//! check moved here from CMuleListCtrl::InsertColumn().
	void RegisterColumn(int index, int defaultWidth, const wxString &name);

	//! Forgets all registered columns (mirrors CMuleListCtrl::ClearAll()).
	void ClearColumns() { m_column_names.clear(); }

	const wxString &GetColumnName(int index) const;
	int GetColumnDefaultWidth(int index) const;
	int GetColumnIndex(const wxString &name) const;

	/**
	 * Last-known width cache for a hidden column, keyed by column index.
	 * Used by the hide/show-via-menu interaction (CMuleListCtrl::OnMenuSelected):
	 * width is cached here when a column is hidden, so it can be restored
	 * when the column is shown again in the same session. 0 if never cached.
	 */
	int GetCachedWidth(int index) const;
	void SetCachedWidth(int index, int width);

	/**
	 * Writes the current column widths/visibility and the given sort
	 * order out to wxConfig, under this store's table name. No-op if no
	 * table name has been set.
	 */
	void SaveSettings(const IColumnWidthProvider &widget, const CSortingList &sortOrders) const;

	/**
	 * Reads column widths/visibility and sort order back from wxConfig,
	 * applying widths directly via the given widget and returning the
	 * decoded sort order (in the same primary-last order
	 * CMuleListCtrl::LoadSettings() has always built it in, ready to be
	 * applied via the caller's own SetSorting()). No-op (and clears
	 * outSortOrders) if no table name has been set.
	 *
	 * @param oldColumnOrder Pre-2.2.2 column order, comma-separated, as
	 * CMuleListCtrl::GetOldColumnOrder() provides; empty to skip that
	 * migration path.
	 */
	void LoadSettings(
		IColumnWidthProvider &widget, const wxString &oldColumnOrder, CSortingList &outSortOrders);

private:
	int GetNewColumnIndex(int oldindex, const wxString &oldColumnOrder) const;
	void ParseOldConfigEntries(const wxString &sortOrders,
		const wxString &columnWidths,
		IColumnWidthProvider &widget,
		const wxString &oldColumnOrder,
		CSortingList &outSortOrders);

	class ColNameEntry
	{
	public:
		int index;
		int defaultWidth;
		wxString name;
		ColNameEntry(int _index, int _defaultWidth, const wxString &_name)
		: index(_index)
		, defaultWidth(_defaultWidth)
		, name(_name)
		{
		}
	};
	typedef std::list<ColNameEntry> ColNameList;

	//! The persistence key prefix. Empty means "don't persist".
	wxString m_name;

	//! Column names, sorted by column index.
	ColNameList m_column_names;

	//! Cache of the columns' sizes (used to remember a hidden column's
	//! last width, per CMuleListCtrl::SaveSettings' original comment).
	typedef std::vector<int> ColSizeVector;
	ColSizeVector m_column_sizes;
};

#endif // LISTCOLUMNSTORE_H
// File_checked_for_headers
