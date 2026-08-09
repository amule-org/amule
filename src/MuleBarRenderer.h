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
	CBarFillSpec(wxUIntPtr identity, uint64 fileSize, std::vector<CBarFillSpan> spans)
	: m_identity(identity)
	, m_fileSize(fileSize)
	, m_spans(std::move(spans))
	{
	}

	/**
	 * The row's own item-data pointer, carried through only so a renderer
	 * subclass can cast it back to its domain type for extra per-list
	 * drawing (e.g. CDownloadBarRenderer -> CPartFile*). Safe to dereference
	 * only synchronously inside Render(): GetValueByRow() is the sole
	 * producer of a CBarFillSpec and only runs for rows currently in the
	 * model, so liveness holds for that call -- this is not a cache key and
	 * must never be stored past one Render() call.
	 */
	wxUIntPtr GetIdentity() const { return m_identity; }
	uint64 GetFileSize() const { return m_fileSize; }
	const std::vector<CBarFillSpan> &GetSpans() const { return m_spans; }

	bool operator==(const CBarFillSpec &other) const
	{
		return m_fileSize == other.m_fileSize && m_spans == other.m_spans;
	}

private:
	wxUIntPtr m_identity = 0;
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

protected:
	//! For a subclass that draws more than the chunk bar (e.g.
	//! CDownloadBarRenderer's completed-strip and percent text) and needs
	//! back the data SetValue() already unpacked.
	const CBarFillSpec &GetSpec() const { return m_spec; }

	/**
	 * The rect CBarShader is actually drawn into for a given cell -- inset by
	 * one pixel a side in round (non-flat) mode, to leave room for the black
	 * border. A subclass overlay (e.g. a completed-progress strip) must draw
	 * into this same rect, not the raw cell, or it will not line up with the
	 * bar underneath it.
	 */
	/**
	 * The area a bar may paint in, one pixel clear of the row above and below.
	 *
	 * Pre-port every caller inset the rect it handed the bar by a pixel top and
	 * bottom before drawing (CSharedFilesCtrl::OnDrawItem and its equivalents),
	 * and the 3D border went on that inset rect, not on the cell. Only the
	 * inner `!bFlat` step survived the port into this class; while GetSize()
	 * reported the text height the clamp in WXCallRender() happened to supply
	 * the missing gap, so nothing showed until the bar started filling its cell
	 * (issue #880). Without it, flat bars in adjacent rows touch and read as one
	 * block, and 3D borders double up into a single thick line.
	 */
	static wxRect InsetForBar(wxRect cell, bool bFlat)
	{
		cell.y++;
		cell.height -= 2;
		if (!bFlat) {
			// Round bar has a black border; the bar sits a pixel inside it.
			cell.x++;
			cell.y++;
			cell.width -= 2;
			cell.height -= 2;
		}
		return cell;
	}

	//! The rect the 3D border is stroked on: the cell less the separating
	//! pixel, so the border does not sit where the gap belongs.
	static wxRect BorderRectForBar(wxRect cell)
	{
		cell.y++;
		cell.height -= 2;
		return cell;
	}

private:
	CBarFillSpec m_spec;
	CBarShader m_bar;
};

#endif // MULEBARRENDERER_H
// File_checked_for_headers
