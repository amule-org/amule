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
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA
//

#ifndef MULEBARRENDERER_H
#define MULEBARRENDERER_H

#include <wx/dataview.h>
#include <wx/variant.h> // Needed for wxDECLARE_VARIANT_OBJECT -- not reliably pulled in
			// transitively by <wx/dataview.h> across wx versions/ports

#include "BarShader.h"  // Needed for CBarShader
#include "MuleColour.h" // Needed for CMuleColour
#include "Types.h"      // Needed for uint64

#include <vector>

/**
 * One coloured span of a CBarShader-style bar, in byte-offset (virtual
 * filesize) space -- exactly what CBarShader::FillRange() takes.
 */
struct CBarFillSpan
{
	uint64 start = 0;
	uint64 end = 0;
	CMuleColour colour;

	bool operator==(const CBarFillSpan &other) const
	{
		return start == other.start && end == other.end && colour.IsSameAs(other.colour);
	}
};

/**
 * The payload a bar-drawing column cell carries through a wxVariant, from a
 * CMuleVirtualDataViewCtrl::GetItemBarFill() implementation to
 * CMuleBarRenderer::Render().
 *
 * Deliberately just data: turning a file's source/availability information
 * into spans is per-list business logic and stays in the list, the same
 * split GetItemColumnText() already has. A CBarFillSpec knows nothing about
 * where its spans came from.
 */
class CBarFillSpec : public wxObject
{
public:
	CBarFillSpec() = default;
	CBarFillSpec(uint64 fileSize, std::vector<CBarFillSpan> spans)
	: m_fileSize(fileSize)
	, m_spans(std::move(spans))
	{
	}

	uint64 GetFileSize() const { return m_fileSize; }
	const std::vector<CBarFillSpan> &GetSpans() const { return m_spans; }

	bool operator==(const CBarFillSpec &other) const
	{
		return m_fileSize == other.m_fileSize && m_spans == other.m_spans;
	}

private:
	uint64 m_fileSize = 0;
	std::vector<CBarFillSpan> m_spans;

	wxDECLARE_DYNAMIC_CLASS(CBarFillSpec);
};

// Declared at namespace scope with the non-"wx"-prefixed macro, not the
// in-class wxDECLARE_VARIANT_OBJECT() used for e.g. wxDataViewIconText:
// wxWidgets 3.2 (the CI's Linux package) only has
// DECLARE_VARIANT_OBJECT/IMPLEMENT_VARIANT_OBJECT, whose expansion is a pair
// of free-function declarations, not friend declarations -- placing them
// inside the class body compiles as ill-formed member operators on 3.2.
// Neither operator touches CBarFillSpec's private members, so free
// functions need no special access and this works on every wx version.
// Confirmed by a build failure against 3.2 that a green build against
// Homebrew's wx 3.3.3 locally didn't catch.
DECLARE_VARIANT_OBJECT(CBarFillSpec)

/**
 * Renders a CBarShader chunk bar in a wxDataViewCtrl cell.
 *
 * List-agnostic: every list feeds it the same CBarFillSpec shape through
 * GetItemBarFill(), so one renderer instance (registered via
 * CMuleDataViewCtrl::AppendBarColumn()) serves any list that wants a bar
 * column. Matches the flat/3D and border-drawing behaviour every existing
 * CBarShader caller already has, since those come from a global preference
 * (thePrefs::UseFlatBar()) rather than anything list-specific.
 *
 * The CBarShader is a renderer member, not a per-Render() local: SetWidth()/
 * SetHeight() reset its content buffer, so it must not be resized more than
 * once per paint, and rebuilding one from scratch per cell per repaint would
 * be wasteful -- the same reason the pre-port code kept it as a function-
 * local static.
 */
class CMuleBarRenderer : public wxDataViewCustomRenderer
{
public:
	CMuleBarRenderer();

	bool SetValue(const wxVariant &value) override;
	bool GetValue(wxVariant &value) const override;

	bool Render(wxRect cell, wxDC *dc, int state) override;
	wxSize GetSize() const override;

private:
	CBarFillSpec m_spec;
	CBarShader m_bar;
};

#endif // MULEBARRENDERER_H
// File_checked_for_headers
