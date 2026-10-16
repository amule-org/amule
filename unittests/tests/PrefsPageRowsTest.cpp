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

#include "PrefsPageRows.h"

using namespace muleunit;

DECLARE_SIMPLE(PrefsPageRows)

TEST(PrefsPageRows, AllPagesShownRowEqualsIndex)
{
	const std::vector<unsigned> shown{ 0, 1, 2, 3, 4 };
	ASSERT_EQUALS(3, PrefsPageRows::Find(shown, 3));
	ASSERT_EQUALS(3u, PrefsPageRows::InsertionRow({ 0, 1, 2, 4 }, 3));
}

TEST(PrefsPageRows, HiddenPageIsNotFound)
{
	ASSERT_EQUALS(-1, PrefsPageRows::Find({ 0, 1, 3 }, 2));
}

// A page hidden above another shifts its row: with page 1 hidden, page 4 sits in row 3, and a
// hidden page 4 goes back in at row 3. Using the pages[] index as the row would hit page 5.
TEST(PrefsPageRows, HiddenPageAboveShiftsRows)
{
	ASSERT_EQUALS(3, PrefsPageRows::Find({ 0, 2, 3, 4, 5 }, 4));
	ASSERT_EQUALS(3u, PrefsPageRows::InsertionRow({ 0, 2, 3, 5 }, 4));
}

TEST(PrefsPageRows, InsertsAtEitherEnd)
{
	ASSERT_EQUALS(0u, PrefsPageRows::InsertionRow({ 1, 2 }, 0));
	ASSERT_EQUALS(2u, PrefsPageRows::InsertionRow({ 0, 1 }, 5));
	ASSERT_EQUALS(0u, PrefsPageRows::InsertionRow({}, 3));
}
