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

#include "ListColumnStore.h"

#include <wx/config.h>
#include <wx/tokenzr.h>

#include <common/StringFunctions.h> // Needed for StrToLong

#include "Types.h" // Needed for EmptyString

namespace
{
// The config format that predates this store wrote CMuleListCtrl::MLOrder
// values verbatim, so reading one back means translating those bits rather
// than assuming they match what this store writes today. Spelled out here
// instead of including MuleListCtrl.h: the store serves both list bases and
// must not adopt either one's vocabulary (see CListColumnStore::SortFlag).
const unsigned long kLegacySortDescending = 0x1000;
const unsigned long kLegacySortAlternate = 0x2000;
const unsigned long kLegacySortMask = kLegacySortDescending | kLegacySortAlternate;
} // namespace

void CListColumnStore::RegisterColumn(int index, int defaultWidth, const wxString &name)
{
#ifdef __DEBUG__
	// Check for valid names
	wxASSERT_MSG(
		name.Find(':') == wxNOT_FOUND, "Column name \"" + name + "\" contains invalid characters!");
	wxASSERT_MSG(
		name.Find(',') == wxNOT_FOUND, "Column name \"" + name + "\" contains invalid characters!");

	// Check for uniqueness of names.
	for (ColNameList::const_iterator uit = m_column_names.begin(); uit != m_column_names.end(); ++uit) {
		if (name == uit->name) {
			wxFAIL_MSG("Column name \"" + name + "\" is not unique!");
		}
	}
#endif
	ColNameList::iterator it = m_column_names.begin();
	while (it != m_column_names.end() && it->index < index) {
		++it;
	}
	m_column_names.insert(it, ColNameEntry(index, defaultWidth, name));
	while (it != m_column_names.end()) {
		++it;
		++(it->index);
	}
}

const wxString &CListColumnStore::GetColumnName(int index) const
{
	for (const ColNameEntry &entry : m_column_names) {
		if (entry.index == index) {
			return entry.name;
		}
	}
	return EmptyString;
}

int CListColumnStore::GetColumnDefaultWidth(int index) const
{
	for (const ColNameEntry &entry : m_column_names) {
		if (entry.index == index) {
			return entry.defaultWidth;
		}
	}
	return -1; // wxLIST_AUTOSIZE
}

int CListColumnStore::GetColumnIndex(const wxString &name) const
{
	for (const ColNameEntry &entry : m_column_names) {
		if (entry.name == name) {
			return entry.index;
		}
	}
	return -1;
}

int CListColumnStore::GetCachedWidth(int index) const
{
	return (index >= 0 && index < (int)m_column_sizes.size()) ? m_column_sizes[index] : 0;
}

void CListColumnStore::SetCachedWidth(int index, int width)
{
	if (index >= (int)m_column_sizes.size()) {
		m_column_sizes.resize(index + 1, 0);
	}
	m_column_sizes[index] = width;
}

int CListColumnStore::GetNewColumnIndex(int oldindex, const wxString &oldColumnOrder) const
{
	wxStringTokenizer oldcolumns(oldColumnOrder, ",", wxTOKEN_RET_EMPTY_ALL);
	while (oldcolumns.HasMoreTokens()) {
		wxString name = oldcolumns.GetNextToken();
		if (oldindex == 0) {
			return GetColumnIndex(name);
		}
		--oldindex;
	}
	return -1;
}

void CListColumnStore::SaveSettings(const IColumnWidthProvider &widget, const CSortingList &sortOrders) const
{
	if (!HasTableName()) {
		return;
	}
	wxConfigBase *cfg = wxConfigBase::Get();

	// Save sorting, column and order
	wxString sortOrder;
	for (CSortingList::const_iterator it = sortOrders.begin(); it != sortOrders.end();) {
		wxString columnName = GetColumnName(static_cast<int>(it->first));
		if (!columnName.IsEmpty()) {
			sortOrder += columnName;
			sortOrder += ":";
			sortOrder += it->second & SORT_DESCENDING ? "1" : "0";
			sortOrder += ":";
			sortOrder += it->second & SORT_ALTERNATE ? "1" : "0";
			if (++it != sortOrders.end()) {
				sortOrder += ",";
			}
		} else {
			++it;
		}
	}
	cfg->Write("/eMule/TableOrdering" + m_name, sortOrder);

	// Save column widths. ATM this is also used to signify hidden columns.
	wxString buffer;
	for (int i = 0; i < widget.GetColumnCount(); ++i) {
		wxString columnName = GetColumnName(i);
		if (!columnName.IsEmpty()) {
			if (!buffer.IsEmpty()) {
				buffer << ",";
			}
			int currentwidth = widget.GetColumnWidth(i);
			int savedsize = (m_column_sizes.size() && (i < (int)m_column_sizes.size()))
						? m_column_sizes[i]
						: 0;
			buffer << columnName << ":" << ((currentwidth > 0) ? currentwidth : (-1 * savedsize));
		}
	}
	cfg->Write("/eMule/TableWidths" + m_name, buffer);
}

bool CListColumnStore::ParseOldConfigEntries(const wxString &sortOrders,
	const wxString &columnWidths,
	IColumnWidthProvider &widget,
	const wxString &oldColumnOrder,
	CSortingList &outSortOrders)
{
	// Set sort order (including sort column)
	wxStringTokenizer tokens(sortOrders, ",");
	while (tokens.HasMoreTokens()) {
		wxString token = tokens.GetNextToken();

		long column = 0;
		unsigned long order = 0;

		if (token.BeforeFirst(' ').Strip(wxString::both).ToLong(&column)) {
			if (token.AfterFirst(' ').Strip(wxString::both).ToULong(&order)) {
				column = GetNewColumnIndex(static_cast<int>(column), oldColumnOrder);
				// Sanity checking, to avoid asserting if column count changes.
				if (column >= 0 && column < widget.GetColumnCount()) {
					// Sanity checking, to avoid asserting if data-format changes.
					if ((order & ~kLegacySortMask) == 0) {
						outSortOrders.emplace_back(column,
							(order & kLegacySortDescending ? SORT_DESCENDING
										       : 0) |
								(order & kLegacySortAlternate ? SORT_ALTERNATE
											      : 0));
					}
				}
			}
		}
	}

	// Set column widths
	bool restoredWidth = false;
	int counter = 0;
	wxStringTokenizer tokenizer(columnWidths, ",");
	while (tokenizer.HasMoreTokens()) {
		long idx = GetNewColumnIndex(counter++, oldColumnOrder);
		long width = StrToLong(tokenizer.GetNextToken());
		if (idx >= 0) {
			widget.SetColumnWidth(static_cast<int>(idx), static_cast<int>(width));
			restoredWidth = true;
		}
	}

	return restoredWidth;
}

bool CListColumnStore::LoadSettings(
	IColumnWidthProvider &widget, const wxString &oldColumnOrder, CSortingList &outSortOrders)
{
	outSortOrders.clear();
	bool restoredWidth = false;
	if (!HasTableName()) {
		return false;
	}
	wxConfigBase *cfg = wxConfigBase::Get();

	wxString sortOrders = cfg->Read("/eMule/TableOrdering" + m_name, "");
	wxString columnWidths = cfg->Read("/eMule/TableWidths" + m_name, "");

	if (columnWidths.Find(':') == wxNOT_FOUND) {
		// Old-style config entries...
		return ParseOldConfigEntries(sortOrders, columnWidths, widget, oldColumnOrder, outSortOrders);
	}

	// Sort orders are stored in order primary, secondary, ...
	// The caller applies them via SetSorting(), which treats the *last*
	// call as primary, so hand them back in reverse (secondary, ..., primary).
	wxStringTokenizer tokens(sortOrders, ",");
	std::list<wxString> tokenList;
	while (tokens.HasMoreTokens()) {
		tokenList.push_front(tokens.GetNextToken());
	}
	for (const wxString &token : tokenList) {
		wxString name = token.BeforeFirst(':');
		long order = StrToLong(token.AfterFirst(':').BeforeLast(':'));
		long alt = StrToLong(token.AfterLast(':'));
		int col = GetColumnIndex(name);
		if (col >= 0) {
			outSortOrders.emplace_back(
				col, (order ? SORT_DESCENDING : 0) | (alt ? SORT_ALTERNATE : 0));
		}
	}

	// Column widths
	wxStringTokenizer tkz(columnWidths, ",");
	while (tkz.HasMoreTokens()) {
		wxString token = tkz.GetNextToken();
		wxString name = token.BeforeFirst(':');
		long width = StrToLong(token.AfterFirst(':'));
		int col = GetColumnIndex(name);
		if (col >= 0) {
			if (col >= (int)m_column_sizes.size()) {
				m_column_sizes.resize(col + 1, 0);
			}
			m_column_sizes[col] = static_cast<int>(abs(width));
			widget.SetColumnWidth(col, static_cast<int>((width > 0) ? width : 0));
			restoredWidth = true;
		}
	}

	return restoredWidth;
}
