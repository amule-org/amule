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

// Country-code resolution stays in the core; this cache is shared by both GUIs.
#include "CountryFlags.h"
#include "icons/icon_data.h"

#include <wx/artprov.h>

CCountryFlags::CCountryFlags() = default;

wxBitmap CCountryFlags::GetFlag(const wxString &code, const wxSize &logicalSize, double contentScale)
{
	wxString key = code;
	auto it = m_flags.find(key);
	if (it == m_flags.end() && !amule_find_icon(("flag_" + key).utf8_str())) {
		key = "unknown";
		it = m_flags.find(key);
	}
	if (it == m_flags.end()) {
		const auto bundle =
			wxArtProvider::GetBitmapBundle("amule:flag_" + key, wxART_OTHER, wxSize(16, 12));
		it = m_flags.emplace(key, bundle).first;
	}
	if (!it->second.IsOk()) {
		return wxNullBitmap;
	}
	const wxSize pixels(wxRound(logicalSize.x * contentScale), wxRound(logicalSize.y * contentScale));
	wxBitmap bitmap = it->second.GetBitmap(pixels);
	if (bitmap.IsOk()) {
		bitmap.SetScaleFactor(contentScale);
	}
	return bitmap;
}
