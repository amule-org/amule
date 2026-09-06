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

#include "OtherFunctions.h" // Needed for StrLang2wx

#include <common/Macros.h> // Needed for itemsof

#include <wx/intl.h>

/**
 * The languages aMule has a translation for.
 *
 * The catalog column is the po/ basename, which is also the directory the catalog
 * installs into. It is spelled out rather than derived from the wx language id
 * because for three languages the two differ: wx canonicalises wxLANGUAGE_ESTONIAN
 * to "et", wxLANGUAGE_KOREAN to "ko" and wxLANGUAGE_PORTUGUESE to "pt", while the
 * catalogs live in et_EE, ko_KR and pt_PT.
 */
static const SLanguageEntry s_languages[] = {
	{ wxLANGUAGE_DEFAULT, "", wxTRANSLATE("System default") },
	{ wxLANGUAGE_ALBANIAN, "sq", wxTRANSLATE("Albanian") },
	{ wxLANGUAGE_ARABIC, "ar", wxTRANSLATE("Arabic") },
	{ wxLANGUAGE_ASTURIAN, "ast", wxTRANSLATE("Asturian") },
	{ wxLANGUAGE_BASQUE, "eu", wxTRANSLATE("Basque") },
	{ wxLANGUAGE_BULGARIAN, "bg", wxTRANSLATE("Bulgarian") },
	{ wxLANGUAGE_CATALAN, "ca", wxTRANSLATE("Catalan") },
	{ wxLANGUAGE_CHINESE_SIMPLIFIED, "zh_CN", wxTRANSLATE("Chinese (Simplified)") },
	{ wxLANGUAGE_CHINESE_TRADITIONAL, "zh_TW", wxTRANSLATE("Chinese (Traditional)") },
	{ wxLANGUAGE_CROATIAN, "hr", wxTRANSLATE("Croatian") },
	{ wxLANGUAGE_CZECH, "cs", wxTRANSLATE("Czech") },
	{ wxLANGUAGE_DANISH, "da", wxTRANSLATE("Danish") },
	{ wxLANGUAGE_DUTCH, "nl", wxTRANSLATE("Dutch") },
	{ wxLANGUAGE_ENGLISH_UK, "en_GB", wxTRANSLATE("English (U.K.)") },
	// The .pot source language: the msgids are the translation, so no catalog ships.
	{ wxLANGUAGE_ENGLISH_US, "", wxTRANSLATE("English (U.S.)") },
	{ wxLANGUAGE_ESTONIAN, "et_EE", wxTRANSLATE("Estonian") },
	{ wxLANGUAGE_FINNISH, "fi", wxTRANSLATE("Finnish") },
	{ wxLANGUAGE_FRENCH, "fr", wxTRANSLATE("French") },
	{ wxLANGUAGE_GALICIAN, "gl", wxTRANSLATE("Galician") },
	{ wxLANGUAGE_GERMAN, "de", wxTRANSLATE("German") },
	{ wxLANGUAGE_GREEK, "el", wxTRANSLATE("Greek") },
	{ wxLANGUAGE_HEBREW, "he", wxTRANSLATE("Hebrew") },
	{ wxLANGUAGE_HUNGARIAN, "hu", wxTRANSLATE("Hungarian") },
	{ wxLANGUAGE_ITALIAN, "it", wxTRANSLATE("Italian") },
	{ wxLANGUAGE_JAPANESE, "ja", wxTRANSLATE("Japanese") },
	{ wxLANGUAGE_KOREAN, "ko_KR", wxTRANSLATE("Korean") },
	{ wxLANGUAGE_LATVIAN, "lv", wxTRANSLATE("Latvian") },
	{ wxLANGUAGE_LITHUANIAN, "lt", wxTRANSLATE("Lithuanian") },
	{ wxLANGUAGE_NORWEGIAN_NYNORSK, "nn", wxTRANSLATE("Norwegian (Nynorsk)") },
	{ wxLANGUAGE_POLISH, "pl", wxTRANSLATE("Polish") },
	{ wxLANGUAGE_PORTUGUESE, "pt_PT", wxTRANSLATE("Portuguese") },
	{ wxLANGUAGE_PORTUGUESE_BRAZILIAN, "pt_BR", wxTRANSLATE("Portuguese (Brazilian)") },
	{ wxLANGUAGE_ROMANIAN, "ro", wxTRANSLATE("Romanian") },
	{ wxLANGUAGE_RUSSIAN, "ru", wxTRANSLATE("Russian") },
	{ wxLANGUAGE_SLOVENIAN, "sl", wxTRANSLATE("Slovenian") },
	{ wxLANGUAGE_SPANISH, "es", wxTRANSLATE("Spanish") },
	{ wxLANGUAGE_SWEDISH, "sv", wxTRANSLATE("Swedish") },
	{ wxLANGUAGE_TURKISH, "tr", wxTRANSLATE("Turkish") },
	{ wxLANGUAGE_UKRAINIAN, "uk", wxTRANSLATE("Ukrainian") },
};

const SLanguageEntry *GetLanguageList(std::size_t &count)
{
	count = itemsof(s_languages);

	return s_languages;
}

int FindLanguageEntry(const wxString &languageID)
{
	const int wxId = StrLang2wx(languageID);

	std::size_t count = 0;
	const SLanguageEntry *languages = GetLanguageList(count);
	for (std::size_t i = 0; i < count; ++i) {
		// A stored value can spell the same language two ways. The picker writes the
		// entry's own id, so Estonian is saved as "et", but a config written by hand,
		// carried over from another install, or copied from the form amulecmd's -l
		// option documents can carry the territory the catalog directory uses,
		// "et_EE". Both name this entry, and matching only the first left the picker
		// unable to find the current language and liable to reset it.
		if (languages[i].wxId == wxId) {
			return static_cast<int>(i);
		}

		if (*languages[i].catalog && StrLang2wx(languages[i].catalog) == wxId) {
			return static_cast<int>(i);
		}
	}

	return -1;
}

bool IsLanguageAvailable(const SLanguageEntry &entry, const wxArrayString &installed)
{
	// "System default" follows the OS and the source language is the msgids
	// themselves, so neither has a catalog to look for.
	if (!*entry.catalog) {
		return true;
	}

	return installed.Index(entry.catalog) != wxNOT_FOUND;
}
