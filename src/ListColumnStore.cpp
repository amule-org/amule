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

#include <algorithm>
#include <cmath>
#include <limits>

#include <wx/config.h>
#include <wx/tokenzr.h>

#include <common/StringFunctions.h> // Needed for StrToLong

#include "Types.h" // Needed for EmptyString

namespace
{
// The config format that predates this store wrote CMuleListCtrl::MLOrder values verbatim, so
// reading one back means translating those bits rather than assuming they match what this store
// writes today. Spelled out here instead of including MuleListCtrl.h: the store serves both list
// bases and must not adopt either one's vocabulary (see CListColumnStore::SortFlag).
const unsigned long kLegacySortDescending = 0x1000;
const unsigned long kLegacySortAlternate = 0x2000;
const unsigned long kLegacySortMask = kLegacySortDescending | kLegacySortAlternate;
// Treat an unavailable/invalid display scale as identity, including for legacy providers.
double WidthScale(const IColumnWidthProvider &widget)
{
	const double scale = widget.GetWidthScale();
	return std::isfinite(scale) && scale > 0.0 ? scale : 1.0;
}
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
	for (const ColNameEntry &entry : m_column_names) {
		if (name == entry.name) {
			wxFAIL_MSG("Column name \"" + name + "\" is not unique!");
		}
	}

	// ... and of indices. Every lookup here (GetColumnName, GetColumnDefaultWidth,
	// GetColumnIndex) matches on index and returns the first entry that does, so a second
	// registration of the same index is not an alternative -- it is dead weight that shadows
	// nothing and reports nothing.
	for (const ColNameEntry &entry : m_column_names) {
		if (index == entry.index) {
			wxFAIL_MSG(wxString::Format(
				"Column index %d is already registered as \"%s\"!", index, entry.name));
		}
	}
#endif
	// Kept sorted by index. Entries are NOT renumbered around an insert: index is the caller's
	// own column id -- a stable #define for the wxDataViewCtrl lists, the wxListCtrl column
	// position for the legacy ones -- and every lookup above matches the value the caller
	// passes back in. Shifting stored indices would break that correspondence, and since a key
	// maps to a column through it, the on-disk TableWidths*/TableOrdering* entries would start
	// addressing the wrong columns.
	ColNameList::iterator it = m_column_names.begin();
	while (it != m_column_names.end() && it->index < index) {
		++it;
	}
	m_column_names.insert(it, ColNameEntry(index, defaultWidth, name));
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
			const int width = currentwidth > 0 ? currentwidth : savedsize;
			const int sign = currentwidth > 0 ? 1 : -1;
			buffer << columnName << ":" << sign * width;
		}
	}
	cfg->Write("/eMule/TableWidths" + m_name, buffer);
	cfg->Write("/eMule/TableWidthsScale" + m_name, WidthScale(widget));
	// Remove companions written by earlier versions of this change.
	cfg->DeleteEntry("/eMule/TableWidthsDIP" + m_name);
	cfg->DeleteEntry("/eMule/TableWidthsDIPSource" + m_name);
}

bool CListColumnStore::RestoreWidth(IColumnWidthProvider &widget, int column, long width, double savedScale)
{
	if (column < 0 || column >= widget.GetColumnCount()) {
		return false;
	}
	// Validate in floating point before negating or narrowing: even LONG_MIN
	// must be harmless. Bound both the logical width and the final widget value.
	const double magnitude = std::abs(static_cast<double>(width));
	if (magnitude > std::numeric_limits<int>::max() || magnitude / savedScale > 4096.0) {
		return false;
	}
	const double currentScale = WidthScale(widget);
	// Same-scale restarts must preserve pixels exactly, including odd widths.
	const double scaled = savedScale == currentScale ? magnitude : magnitude / savedScale * currentScale;
	if (!std::isfinite(scaled) || scaled > std::numeric_limits<int>::max() - 0.5) {
		return false;
	}
	const int pixels = static_cast<int>(std::round(scaled));
	SetCachedWidth(column, pixels > COL_SIZE_MIN ? pixels : GetColumnDefaultWidth(column));
	// Never restore a visible column narrower than its current header: a width saved under
	// another language, an older default, or a narrower user drag would otherwise clip the
	// header text (e.g. Italian "Dimensione" in 60 px saved for English "Size") with no way
	// back except a manual resize. Hidden columns (saved width <= 0) keep their state.
	const int minimum = std::max(COL_SIZE_MIN + 1, widget.GetColumnMinimumWidth(column));
	widget.SetColumnWidth(column, width > 0 ? std::max(minimum, pixels) : 0);
	return true;
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
		long width = 0;
		if (tokenizer.GetNextToken().ToLong(&width) && idx >= 0) {
			restoredWidth =
				RestoreWidth(widget, static_cast<int>(idx), width, WidthScale(widget)) ||
				restoredWidth;
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

	// Sort orders are stored primary, secondary, ... The caller applies them via SetSorting(),
	// which treats the *last* call as primary, so hand them back in reverse.
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

	// An absent or invalid scale leaves legacy pixels unchanged: their original
	// display scale is unknown. The next save records the current scale.
	double savedScale = 0.0;
	if (!cfg->Read("/eMule/TableWidthsScale" + m_name, &savedScale) || !std::isfinite(savedScale) ||
		savedScale <= 0.0) {
		savedScale = WidthScale(widget);
	}
	wxStringTokenizer tkz(columnWidths, ",");
	while (tkz.HasMoreTokens()) {
		wxString token = tkz.GetNextToken();
		wxString name = token.BeforeFirst(':');
		long width = 0;
		const int col = GetColumnIndex(name);
		if (token.AfterFirst(':').ToLong(&width)) {
			restoredWidth = RestoreWidth(widget, col, width, savedScale) || restoredWidth;
		}
	}

	return restoredWidth;
}
