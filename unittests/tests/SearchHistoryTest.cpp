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

#include <muleunit/test.h>

#include <SearchHistory.h>

using namespace muleunit;

DECLARE_SIMPLE(SearchHistory)

TEST(SearchHistory, NewTermPrependedToEmptyList)
{
	wxArrayString result = ApplySearchHistoryEntry(wxArrayString(), "ubuntu", 30);

	ASSERT_EQUALS((size_t)1, result.GetCount());
	ASSERT_EQUALS(wxString("ubuntu"), result[0]);
}

TEST(SearchHistory, NewTermPrependedAheadOfExisting)
{
	wxArrayString existing;
	existing.Add("debian");
	existing.Add("fedora");

	wxArrayString result = ApplySearchHistoryEntry(existing, "ubuntu", 30);

	ASSERT_EQUALS((size_t)3, result.GetCount());
	ASSERT_EQUALS(wxString("ubuntu"), result[0]);
	ASSERT_EQUALS(wxString("debian"), result[1]);
	ASSERT_EQUALS(wxString("fedora"), result[2]);
}

TEST(SearchHistory, DuplicateIsMovedToFrontNotAppended)
{
	wxArrayString existing;
	existing.Add("debian");
	existing.Add("ubuntu");
	existing.Add("fedora");

	wxArrayString result = ApplySearchHistoryEntry(existing, "ubuntu", 30);

	ASSERT_EQUALS((size_t)3, result.GetCount());
	ASSERT_EQUALS(wxString("ubuntu"), result[0]);
	ASSERT_EQUALS(wxString("debian"), result[1]);
	ASSERT_EQUALS(wxString("fedora"), result[2]);
}

TEST(SearchHistory, DedupIsCaseInsensitive)
{
	wxArrayString existing;
	existing.Add("Ubuntu");

	wxArrayString result = ApplySearchHistoryEntry(existing, "ubuntu", 30);

	// The freshly-typed casing wins; the stale-cased entry is gone, not
	// just reordered.
	ASSERT_EQUALS((size_t)1, result.GetCount());
	ASSERT_EQUALS(wxString("ubuntu"), result[0]);
}

TEST(SearchHistory, EmptyTermLeavesListUnchanged)
{
	wxArrayString existing;
	existing.Add("debian");
	existing.Add("fedora");

	wxArrayString result = ApplySearchHistoryEntry(existing, "", 30);

	ASSERT_EQUALS((size_t)2, result.GetCount());
	ASSERT_EQUALS(wxString("debian"), result[0]);
	ASSERT_EQUALS(wxString("fedora"), result[1]);
}

TEST(SearchHistory, ResultIsCappedAtMaxEntries)
{
	wxArrayString existing;
	for (int i = 0; i < 5; ++i) {
		existing.Add(wxString::Format("term%d", i));
	}

	wxArrayString result = ApplySearchHistoryEntry(existing, "newest", 3);

	ASSERT_EQUALS((size_t)3, result.GetCount());
	ASSERT_EQUALS(wxString("newest"), result[0]);
	ASSERT_EQUALS(wxString("term0"), result[1]);
	ASSERT_EQUALS(wxString("term1"), result[2]);
	// term2..term4 fell off the tail -- oldest entries drop first.
}

TEST(SearchHistory, CapIsExactlyThirtyByProjectConvention)
{
	// Locks in eMule's CCustomAutoComplete default (amule-org/amule#643
	// review) as the constant CSearchDlg actually passes in, not just
	// that ApplySearchHistoryEntry can cap at an arbitrary N.
	wxArrayString existing;
	for (int i = 0; i < 30; ++i) {
		existing.Add(wxString::Format("term%d", i));
	}

	wxArrayString result = ApplySearchHistoryEntry(existing, "newest", 30);

	ASSERT_EQUALS((size_t)30, result.GetCount());
	ASSERT_EQUALS(wxString("newest"), result[0]);
	ASSERT_EQUALS(wxString("term28"), result[29]);
}
