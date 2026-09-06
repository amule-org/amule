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

#include "LanguageList.h"

#include "OtherFunctions.h"

#include <muleunit/test.h>

#include <wx/dir.h>
#include <wx/filename.h>
#include <wx/intl.h>

using namespace muleunit;

DECLARE_SIMPLE(LanguageList)

//! Catalogs that ship but are deliberately not offered in the picker, because they
//! hold (next to) no translated strings: offering them would show an English UI
//! under a native-language label. Move one here into GetLanguageList() once it is
//! actually translated.
static const char *const s_unlistedCatalogs[] = { "bn", "ta", "vi" };

static bool IsUnlisted(const wxString &catalog)
{
	for (const char *unlisted : s_unlistedCatalogs) {
		if (catalog == unlisted) {
			return true;
		}
	}

	return false;
}

static wxArrayString PoBasenames()
{
	wxArrayString names;
	wxDir dir(wxSTRINGIZE_T(PO_SRCDIR));
	if (!dir.IsOpened()) {
		return names;
	}

	wxString file;
	for (bool ok = dir.GetFirst(&file, "*.po", wxDIR_FILES); ok; ok = dir.GetNext(&file)) {
		names.Add(wxFileName(file).GetName());
	}

	return names;
}

// The picker matches a catalog by name, so an entry naming a po file that is not
// there is a language silently missing from Preferences.
TEST(LanguageList, EveryListedCatalogExists)
{
	const wxArrayString po = PoBasenames();
	ASSERT_TRUE(!po.IsEmpty());

	std::size_t count = 0;
	const SLanguageEntry *languages = GetLanguageList(count);
	for (std::size_t i = 0; i < count; ++i) {
		if (!*languages[i].catalog) {
			continue;
		}

		CONTEXT(wxString("Listed language: ") + languages[i].name);
		ASSERT_TRUE_M(po.Index(languages[i].catalog) != wxNOT_FOUND,
			wxString("No po/") + languages[i].catalog + ".po for " + languages[i].name);
	}
}

// The other direction: a translation added to po/ without a list entry never
// reaches the picker. Failing here is the prompt to add it, or to record it as a
// deliberately unlisted stub.
TEST(LanguageList, EveryCatalogIsListedOrDeliberatelyNot)
{
	const wxArrayString po = PoBasenames();
	ASSERT_TRUE(!po.IsEmpty());

	std::size_t count = 0;
	const SLanguageEntry *languages = GetLanguageList(count);
	for (const wxString &catalog : po) {
		CONTEXT(wxString("Catalog: ") + catalog);

		bool listed = false;
		for (std::size_t j = 0; j < count && !listed; ++j) {
			listed = (catalog == languages[j].catalog);
		}

		ASSERT_TRUE_M(listed || IsUnlisted(catalog),
			wxString("po/") + catalog +
				".po is neither in GetLanguageList() nor recorded as an "
				"untranslated stub");
	}
}

// FindLanguageEntry() accepts a language under two names, so no two entries may
// answer to the same one.
TEST(LanguageList, NoTwoEntriesClaimTheSameLanguage)
{
	std::size_t count = 0;
	const SLanguageEntry *languages = GetLanguageList(count);
	for (std::size_t i = 0; i < count; ++i) {
		CONTEXT(wxString("Entry: ") + languages[i].name);

		ASSERT_EQUALS(static_cast<int>(i), FindLanguageEntry(wxLang2Str(languages[i].wxId)));

		if (*languages[i].catalog) {
			ASSERT_EQUALS(static_cast<int>(i), FindLanguageEntry(languages[i].catalog));
		}
	}
}

// The three languages whose catalog directory is not the wx canonical name. A
// config can spell these either way, and both must land on the same entry.
TEST(LanguageList, BothSpellingsFindTheSameEntry)
{
	ASSERT_EQUALS(FindLanguageEntry("et"), FindLanguageEntry("et_EE"));
	ASSERT_EQUALS(FindLanguageEntry("ko"), FindLanguageEntry("ko_KR"));
	ASSERT_EQUALS(FindLanguageEntry("pt"), FindLanguageEntry("pt_PT"));

	// Portuguese and its Brazilian variant stay apart.
	ASSERT_TRUE(FindLanguageEntry("pt_PT") != FindLanguageEntry("pt_BR"));
}

// An empty or unrecognised value is what a fresh or hand-broken config holds, and
// the app runs it as the system language.
TEST(LanguageList, AnUnknownLanguageIsTheSystemDefault)
{
	std::size_t count = 0;
	const SLanguageEntry *languages = GetLanguageList(count);
	ASSERT_TRUE(count > 0);
	ASSERT_EQUALS(wxLANGUAGE_DEFAULT, languages[0].wxId);

	ASSERT_EQUALS(0, FindLanguageEntry(""));
	ASSERT_EQUALS(0, FindLanguageEntry("no-such-language"));
}

TEST(LanguageList, AvailabilityFollowsTheInstalledCatalogs)
{
	wxArrayString installed;
	installed.Add("de");
	installed.Add("et_EE");

	std::size_t count = 0;
	const SLanguageEntry *languages = GetLanguageList(count);

	const int german = FindLanguageEntry("de");
	const int estonian = FindLanguageEntry("et_EE");
	const int italian = FindLanguageEntry("it");
	ASSERT_TRUE(german >= 0 && estonian >= 0 && italian >= 0);

	ASSERT_TRUE(IsLanguageAvailable(languages[german], installed));
	ASSERT_TRUE(IsLanguageAvailable(languages[estonian], installed));
	ASSERT_FALSE(IsLanguageAvailable(languages[italian], installed));

	// System default and the source language ship no catalog and are always offered.
	for (std::size_t i = 0; i < count; ++i) {
		if (!*languages[i].catalog) {
			CONTEXT(wxString("Catalog-less entry: ") + languages[i].name);
			ASSERT_TRUE(IsLanguageAvailable(languages[i], installed));
			ASSERT_TRUE(IsLanguageAvailable(languages[i], wxArrayString()));
		}
	}
}
