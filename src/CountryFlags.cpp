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
#include "IconDensities.h"
#include "icons/icon_data.h"

#include <cmath>
#include <cstring>

namespace
{
const AMuleIconEntry *FindFlag(const wxString &code)
{
	// Index the fixed embedded inventory once; unknown network codes never grow it.
	static const auto entries = [] {
		std::map<wxString, const AMuleIconEntry *> result;
		int count = 0;
		const auto icons = amule_get_all_icons(&count);
		for (int i = 0; i < count; ++i) {
			if (std::strncmp(icons[i].name, "flag_", 5) == 0) {
				result.emplace(wxString::FromUTF8(icons[i].name + 5), &icons[i]);
			}
		}
		return result;
	}();
	const auto found = entries.find(code);
	const auto fallback = entries.find("unknown");
	return found != entries.end()      ? found->second
	       : fallback != entries.end() ? fallback->second
					   : nullptr;
}
} // namespace

wxBitmap CCountryFlags::GetFlag(const wxString &code, const wxSize &logicalSize, double contentScale)
{
	if (logicalSize.x <= 0 || logicalSize.y <= 0 || !std::isfinite(contentScale) || contentScale <= 0) {
		return wxNullBitmap;
	}
	const auto entry = FindFlag(code);
	if (!entry) {
		return wxNullBitmap;
	}
	const wxString key = wxString::FromUTF8(entry->name);
	auto it = m_flags.find(key);
	if (it == m_flags.end()) {
		FlagArtwork artwork;
		artwork.images = LoadIconDensities(*entry);
		it = m_flags.emplace(key, artwork).first;
	}
	auto &artwork = it->second;
	const auto sizeKey = std::make_tuple(logicalSize.x, logicalSize.y, contentScale);
	const auto cached = artwork.bitmaps.find(sizeKey);
	if (cached != artwork.bitmaps.end()) {
		return cached->second;
	}
	const wxSize pixels(wxRound(logicalSize.x * contentScale), wxRound(logicalSize.y * contentScale));
	if (pixels.x <= 0 || pixels.y <= 0) {
		return wxNullBitmap;
	}
	const wxImage image = ScaleIconDensity(artwork.images, pixels);
	if (!image.IsOk()) {
		return wxNullBitmap;
	}
	wxBitmap bitmap(image);
	if (bitmap.IsOk()) {
		// SetScaleFactor may copy pixel data. Do it only for a new scale, then
		// share the completed bitmap on subsequent list-cell draws.
		bitmap.SetScaleFactor(contentScale);
		if (artwork.bitmaps.size() >= 8) {
			artwork.bitmaps.erase(artwork.bitmaps.begin());
		}
		artwork.bitmaps.emplace(sizeKey, bitmap);
	}
	return bitmap;
}
