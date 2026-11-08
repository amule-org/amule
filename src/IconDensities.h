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

#ifndef ICONDENSITIES_H
#define ICONDENSITIES_H

#include <array>

#include <wx/image.h>
#include <wx/mstream.h>

#include "icons/icon_data.h"

// The 1x, 2x and 3x PNGs of an embedded icon; a missing density stays invalid.
inline std::array<wxImage, 3> LoadIconDensities(const AMuleIconEntry &entry)
{
	std::array<wxImage, 3> images;
	const unsigned char *data[] = { entry.png_data, entry.png2x_data, entry.png3x_data };
	const unsigned int lengths[] = { entry.png_len, entry.png2x_len, entry.png3x_len };
	for (size_t i = 0; i < images.size(); ++i) {
		if (!data[i] || !lengths[i]) {
			continue;
		}
		wxMemoryInputStream stream(data[i], lengths[i]);
		auto &image = images[i];
		// A mask becomes alpha: high-quality scaling smears a mask colour into the edges.
		if (image.LoadFile(stream, wxBITMAP_TYPE_PNG) && !image.HasAlpha()) {
			image.InitAlpha();
		}
	}
	return images;
}

// The smallest density covering `pixels`, else the largest, resampled to exactly that size.
inline wxImage ScaleIconDensity(const std::array<wxImage, 3> &images, const wxSize &pixels)
{
	const wxImage *source = nullptr;
	for (const auto &image : images) {
		if (!image.IsOk()) {
			continue;
		}
		source = &image;
		if (image.GetWidth() >= pixels.x && image.GetHeight() >= pixels.y) {
			break;
		}
	}
	if (!source) {
		return wxImage();
	}
	return source->GetSize() == pixels ? *source
					   : source->Scale(pixels.x, pixels.y, wxIMAGE_QUALITY_HIGH);
}

#endif // ICONDENSITIES_H
