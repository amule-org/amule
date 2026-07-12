//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
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

//
// Country flags are from FAMFAMFAM (http://www.famfamfam.com) — public domain,
// named with the ISO 3166-1 alpha-2 country codes.
//

#ifndef COUNTRYFLAGS_H
#define COUNTRYFLAGS_H

#include <map>

#include <wx/image.h>
#include <wx/string.h>

// GUI-only country flag cache: maps an ISO 3166-1 alpha-2 code (lowercase) to
// its flag bitmap. Split out of CIP2Country so the resolver stays headless and
// usable in amuled (see IP2Country.h) — country *codes* travel over EC, and
// each GUI turns the code into a flag here. Owned by CamuleGuiBase, so both the
// monolithic and remote GUIs share one instance.
class CCountryFlags
{
public:
	CCountryFlags();

	// Flag image for an ISO code (lowercase). Returns the "unknown" (??)
	// flag when the code is empty or has no bundled image.
	const wxImage &GetFlag(const wxString &code);

private:
	// Lazily populate m_flags on first GetFlag(). Deferred (not done in the
	// ctor) because CamuleGuiBase constructs this before the app's OnInit has
	// pushed CamuleArtProvider — loading in the ctor would find no art and
	// every flag would come back blank (peers list shows the bare code, no
	// flag). By first draw, OnInit has run and the provider is live.
	void LoadFlags();

	bool m_loaded = false;
	std::map<wxString, wxImage> m_flags;
	wxImage m_unknown;
};

#endif // COUNTRYFLAGS_H
