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

#include "PrefsSearch.h"

using namespace muleunit;

DECLARE_SIMPLE(PrefsSearch)

TEST(PrefsSearch, FoldIgnoresCase)
{
	ASSERT_EQUALS(wxString("connection"), PrefsSearch::Fold("CONNECTION"));
}

// Translated labels are searched, and users type without accents.
TEST(PrefsSearch, FoldDropsAccents)
{
	ASSERT_EQUALS(wxString("conexion"), PrefsSearch::Fold(L"Conexi\u00f3n"));
	ASSERT_EQUALS(wxString("francais"), PrefsSearch::Fold(L"Fran\u00e7ais"));
	ASSERT_EQUALS(wxString("ubersicht"), PrefsSearch::Fold(L"\u00dcbersicht"));
	ASSERT_EQUALS(wxString("zrodlo"), PrefsSearch::Fold(L"\u0179r\u00f3d\u0142o"));
}

// A label wrapped over several lines must still match a phrase that spans the break.
TEST(PrefsSearch, FoldCollapsesWhitespace)
{
	ASSERT_EQUALS(wxString("start minimized"), PrefsSearch::Fold("  Start\n   minimized \t"));
}

TEST(PrefsSearch, TermsSplitsOnWhitespace)
{
	const std::vector<wxString> terms = PrefsSearch::Terms("  Tray   ICON ");
	ASSERT_EQUALS(2u, terms.size());
	ASSERT_EQUALS(wxString("tray"), terms[0]);
	ASSERT_EQUALS(wxString("icon"), terms[1]);
	ASSERT_TRUE(PrefsSearch::Terms(" \t ").empty());
}

TEST(PrefsSearch, MatchesNeedsEveryTermInAnyOrder)
{
	const wxString label = "Minimize to Tray Icon";
	ASSERT_TRUE(PrefsSearch::Matches(label, PrefsSearch::Terms("tray")));
	ASSERT_TRUE(PrefsSearch::Matches(label, PrefsSearch::Terms("icon minim")));
	ASSERT_FALSE(PrefsSearch::Matches(label, PrefsSearch::Terms("tray port")));
}

// A term has to start a word, or a short one matches inside unrelated words.
TEST(PrefsSearch, MatchesAtWordStartsOnly)
{
	ASSERT_TRUE(PrefsSearch::Matches("Standard TCP Port", PrefsSearch::Terms("port")));
	ASSERT_TRUE(PrefsSearch::Matches("UPnP ports (optional)", PrefsSearch::Terms("port")));
	ASSERT_TRUE(PrefsSearch::Matches("Use (eD2k) links", PrefsSearch::Terms("ed2k")));
	ASSERT_FALSE(
		PrefsSearch::Matches("BitTorrent magnets are not supported", PrefsSearch::Terms("port")));
	// The first two "port"s sit inside words; the search goes on to the one that starts one.
	ASSERT_TRUE(PrefsSearch::Matches("Not supported; support ports", PrefsSearch::Terms("supp port")));
}

// Without spaces between words there is nothing to anchor to, so CJK terms match anywhere.
TEST(PrefsSearch, MatchesInsideUnspacedScripts)
{
	ASSERT_TRUE(
		PrefsSearch::Matches(L"\u4ee3\u7406\u670d\u52a1\u5668", PrefsSearch::Terms(L"\u670d\u52a1")));
}

TEST(PrefsSearch, MatchesNothingWithoutTerms)
{
	ASSERT_FALSE(PrefsSearch::Matches("Anything", PrefsSearch::Terms("")));
}
