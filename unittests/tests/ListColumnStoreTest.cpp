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

// CListColumnStore maps a column's id to its persistence key and default width.
// The key is the on-disk config format (TableWidths*/TableOrdering*), so the id
// a caller registers has to be the id it can look the column back up by -- if
// the store were to renumber stored ids, a saved width would come back attached
// to a different column, silently.
//
// These tests pin that the registry is a plain sorted insert. Registering out of
// order is the case that matters: it never happens in the app today, because
// every list registers its columns in ascending order, which is exactly why a
// renumbering bug in this path could sit here unnoticed.

#include <muleunit/test.h>

#include <ListColumnStore.h>

using namespace muleunit;

DECLARE_SIMPLE(CListColumnStore)

TEST(CListColumnStore, RegistrationOutOfOrderKeepsEveryColumnAddressable)
{
	// The middle insert: 1 lands between columns already registered, which is
	// the shape a renumbering insert would corrupt -- moving 2 and 3 out from
	// under the callers that still ask for them by those ids.
	//
	// Two entries have to follow the insertion point for that to be visible.
	// With only one, an off-by-one in such a loop skips the single displaced
	// entry and the damage lands somewhere unobservable instead.
	CListColumnStore store;
	store.RegisterColumn(0, 250, "N");
	store.RegisterColumn(2, 100, "Z");
	store.RegisterColumn(3, 90, "Y");
	store.RegisterColumn(1, 70, "P");

	ASSERT_EQUALS(wxString("N"), store.GetColumnName(0));
	ASSERT_EQUALS(wxString("P"), store.GetColumnName(1));
	ASSERT_EQUALS(wxString("Z"), store.GetColumnName(2));
	ASSERT_EQUALS(wxString("Y"), store.GetColumnName(3));

	ASSERT_EQUALS(250, store.GetColumnDefaultWidth(0));
	ASSERT_EQUALS(70, store.GetColumnDefaultWidth(1));
	ASSERT_EQUALS(100, store.GetColumnDefaultWidth(2));
	ASSERT_EQUALS(90, store.GetColumnDefaultWidth(3));
}

TEST(CListColumnStore, KeyLookupSurvivesOutOfOrderRegistration)
{
	// The reverse direction, which is how a saved config finds its column:
	// LoadSettings() reads a key and calls GetColumnIndex() to place it.
	CListColumnStore store;
	store.RegisterColumn(0, 250, "N");
	store.RegisterColumn(2, 100, "Z");
	store.RegisterColumn(1, 70, "P");

	ASSERT_EQUALS(0, store.GetColumnIndex("N"));
	ASSERT_EQUALS(1, store.GetColumnIndex("P"));
	ASSERT_EQUALS(2, store.GetColumnIndex("Z"));
}

TEST(CListColumnStore, AscendingRegistrationIsUnchanged)
{
	// What every list in the app actually does; guards the common path against
	// a regression in the insert itself.
	CListColumnStore store;
	store.RegisterColumn(0, 400, "N");
	store.RegisterColumn(1, 100, "Z");
	store.RegisterColumn(2, 90, "Y");

	ASSERT_EQUALS(wxString("N"), store.GetColumnName(0));
	ASSERT_EQUALS(wxString("Z"), store.GetColumnName(1));
	ASSERT_EQUALS(wxString("Y"), store.GetColumnName(2));

	ASSERT_EQUALS(400, store.GetColumnDefaultWidth(0));
	ASSERT_EQUALS(100, store.GetColumnDefaultWidth(1));
	ASSERT_EQUALS(90, store.GetColumnDefaultWidth(2));

	ASSERT_EQUALS(0, store.GetColumnIndex("N"));
	ASSERT_EQUALS(1, store.GetColumnIndex("Z"));
	ASSERT_EQUALS(2, store.GetColumnIndex("Y"));
}

TEST(CListColumnStore, NonContiguousIdsAreKeptAsGiven)
{
	// Ids need not be dense: the store must not compact them into positions.
	CListColumnStore store;
	store.RegisterColumn(5, 120, "T");
	store.RegisterColumn(9, 130, "H");

	ASSERT_EQUALS(wxString("T"), store.GetColumnName(5));
	ASSERT_EQUALS(wxString("H"), store.GetColumnName(9));
	ASSERT_EQUALS(5, store.GetColumnIndex("T"));
	ASSERT_EQUALS(9, store.GetColumnIndex("H"));
}

TEST(CListColumnStore, UnknownColumnsReportTheirMissValues)
{
	CListColumnStore store;
	store.RegisterColumn(0, 250, "N");

	ASSERT_TRUE(store.GetColumnName(7).IsEmpty());
	ASSERT_EQUALS(-1, store.GetColumnDefaultWidth(7)); // wxLIST_AUTOSIZE
	ASSERT_EQUALS(-1, store.GetColumnIndex("nope"));
}
