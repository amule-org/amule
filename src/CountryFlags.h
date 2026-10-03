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
// Artwork provenance and licenses: icons/ARTWORK.md.
//

#ifndef COUNTRYFLAGS_H
#define COUNTRYFLAGS_H

#include <map>

#include <wx/bmpbndl.h>
#include <wx/string.h>

// GUI-only country flag cache: maps an ISO 3166-1 alpha-2 code (lowercase) to its flag bitmap.
// Split out of CIP2Country so the resolver stays headless and usable in amuled (see IP2Country.h)
// -- country *codes* travel over EC, and each GUI turns the code into a flag here. Owned by
// CamuleGuiBase, so both the monolithic and remote GUIs share one instance.
class CCountryFlags
{
public:
	CCountryFlags();

	// Flag image for an ISO code (lowercase). Returns the "unknown" (??)
	// flag when the code is empty or has no bundled image.
	// logicalSize is in drawing coordinates; contentScale is the backing-store scale.
	// Callers use FromDIP(wxSize(16, 12)) and their window/DC's GetContentScaleFactor().
	wxBitmap GetFlag(const wxString &code, const wxSize &logicalSize, double contentScale);

private:
	// Load only flags actually displayed, after the art provider has been registered.
	// Retain vector bundles rather than one raster size, so moving between monitors works.
	std::map<wxString, wxBitmapBundle> m_flags;
};

#endif // COUNTRYFLAGS_H
