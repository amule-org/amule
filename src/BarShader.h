//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2002-2011 Merkur ( devs@emule-project.net / http://www.emule-project.net )
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

#ifndef BARSHADER_H
#define BARSHADER_H

#include "Types.h" // Needed for uint16 and uint32
#include "MuleColour.h"

class wxRect;
class wxDC;

/**
 * Draws the chunk-based progress bars used in aMule. A file's chunks are represented as spans, each
 * covering a range with a colour. New spans can be added on the fly; old ones are automatically
 * removed, resized or merged as needed, and the number of spans is minimised where possible.
 */
class CBarShader
{
public:
	/**
	 * @param height The height of the area the span is drawn on.
	 * @param width The width of the area the span is drawn on.
	 */
	CBarShader(unsigned height = 1, unsigned width = 1);

	~CBarShader();

	/**
	 * Sets the width of the drawn bar, and resets the pixel buffer to the fill colour.
	 */
	void SetWidth(int width);

	/**
	 * Sets the height of the drawn bar.
	 */
	void SetHeight(unsigned height);

	/**
	 * Sets the 3D depth of the bar. @param depth A value from 1 to 5.
	 */
	void Set3dDepth(unsigned depth);

	/**
	 * Sets a new filesize, which is the virtual length of the bar. Must be called before any
	 * filling.
	 */
	void SetFileSize(uint64 fileSize) { m_FileSize = fileSize; }

	/**
	 * Fills the range [@a start, @a end) with @a colour. Any span the new one completely or
	 * partially covers is removed or resized. If @a end is past the current filesize, the
	 * filesize grows to it. @a end must be larger than @a start.
	 */
	void FillRange(uint64 start, uint64 end, const CMuleColour &colour);

	/**
	 * Fills the entire bar with a span of @a colour.
	 */
	void Fill(const CMuleColour &colour)
	{
		m_Content.clear();
		m_Content.resize(m_Width, colour);
	}

	/**
	 * Draws the bar on @a dc at (@a iLeft, @a iTop), with the height and width set through the
	 * constructor or SetWidth()/SetHeight(). @a bFlat suppresses the 3D effect.
	 */
	void Draw(wxDC *dc, int iLeft, int iTop, bool bFlat);

private:
	/**
	 * Calculates the modifiers used to create the 3d effect.
	 */
	void BuildModifiers();

	//! The width of the drawn bar
	unsigned m_Width;
	//! The height of the drawn bar
	unsigned m_Height;
	//! The virtual filesize associated with the bar
	uint64 m_FileSize;
	//! Pointer to array of modifiers used to create 3D effect. Size is (m_Height+1)/2 when set.
	double *m_Modifiers;
	//! The current 3d level
	uint16 m_used3dlevel;

	// color for each pixel across the width is stored here
	std::vector<CMuleColour> m_Content;
};

#endif
// File_checked_for_headers
