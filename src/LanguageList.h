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

#ifndef LANGUAGELIST_H
#define LANGUAGELIST_H

#include <wx/arrstr.h>
#include <wx/string.h>

#include <cstddef>

/**
 * One language offered by the Preferences language picker.
 */
struct SLanguageEntry
{
	//! wx language id. This is what ends up in the /eMule/Language config key,
	//! spelled by wxLang2Str(), and what InitLocale() is handed on startup.
	int wxId;
	//! Catalog name: po/<catalog>.po in the source tree, installed as
	//! <catalog>/LC_MESSAGES/amule.mo. Empty for the entries that ship no catalog.
	//!
	//! This is stored rather than derived from wxId because the two disagree:
	//! wxLocale::GetLanguageInfo(wxLANGUAGE_ESTONIAN)->CanonicalName is "et" while
	//! the catalog directory is "et_EE". Deriving it silently loses Estonian,
	//! Korean and Portuguese.
	const char *catalog;
	//! English display name, run through wxGetTranslation() when shown.
	const char *name;
};

/**
 * The languages aMule ships a translation for.
 *
 * Add new languages here, with the po/<catalog>.po that carries them.
 * LanguageListTest keeps this list and po/ in step.
 *
 * @param count Receives the number of entries.
 * @return The entries, starting with "System default".
 */
const SLanguageEntry *GetLanguageList(std::size_t &count);

/**
 * Finds the entry a stored /eMule/Language value names.
 *
 * @param languageID The stored value, as written by wxLang2Str().
 * @return Index into GetLanguageList(), or -1 when nothing matches.
 */
int FindLanguageEntry(const wxString &languageID);

/**
 * Tells whether an entry's translation is installed.
 *
 * @param entry The entry to check.
 * @param installed Catalog names present on disk, as returned by
 *                  wxTranslations::GetAvailableTranslations().
 * @return true when the entry can be offered. Entries without a catalog
 *         ("System default" and the source language) are always available.
 */
bool IsLanguageAvailable(const SLanguageEntry &entry, const wxArrayString &installed);

#endif /* LANGUAGELIST_H */
