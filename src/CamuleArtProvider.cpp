// CamuleArtProvider -- see CamuleArtProvider.h for the contract.

#include "CamuleArtProvider.h"

#include "icons/icon_data.h"

#include <wx/bitmap.h>
#include <wx/bmpbndl.h> // Needed for wxBitmapBundle::FromSVG / FromBitmaps
#include <wx/image.h>
#include <wx/mstream.h>

const wxString CamuleArtProvider::PREFIX = "amule:";

wxBitmap CamuleArtProvider::CreateBitmap(
	const wxArtID &id, const wxArtClient &WXUNUSED(client), const wxSize &size)
{
	// Only resolve our own art ids; let other providers handle the rest.
	if (!id.StartsWith(PREFIX)) {
		return wxNullBitmap;
	}

	const wxString short_name = id.Mid(PREFIX.length());
	const struct AMuleIconEntry *entry = amule_find_icon(short_name.utf8_str().data());
	if (entry == NULL) {
		return wxNullBitmap;
	}

	// Decode the embedded PNG bytes. wxImage::LoadFile via a wxMemoryInputStream wins over
	// wxBitmap::NewFromPNGData here because we may need to rescale below, and Scale() is on
	// wxImage, not wxBitmap.
	wxMemoryInputStream stream(entry->png_data, entry->png_len);
	wxImage image;
	if (!image.LoadFile(stream, wxBITMAP_TYPE_PNG)) {
		return wxNullBitmap;
	}

	// Honour an explicit size request from the caller. GetBitmap() passes wxDefaultSize when
	// the caller does not care; then the natural size from the PNG header wins.
	if (size != wxDefaultSize &&
		(size.GetWidth() != image.GetWidth() || size.GetHeight() != image.GetHeight())) {
		image = image.Scale(size.GetWidth(), size.GetHeight(), wxIMAGE_QUALITY_HIGH);
	}

	return wxBitmap(image);
}

wxBitmapBundle CamuleArtProvider::CreateBitmapBundle(
	const wxArtID &id, const wxArtClient &WXUNUSED(client), const wxSize &size)
{
	if (!id.StartsWith(PREFIX)) {
		return wxBitmapBundle();
	}

	const wxString short_name = id.Mid(PREFIX.length());
	const struct AMuleIconEntry *entry = amule_find_icon(short_name.utf8_str().data());
	if (entry == nullptr) {
		return wxBitmapBundle();
	}

#ifdef wxHAS_SVG
	if (entry->svg_data != nullptr && entry->svg_len > 0) {
		// FromSVG needs an explicit default (logical) size; when the
		// caller doesn't request one, the PNG twin's natural size wins.
		wxSize sizeDef(size);
		if (sizeDef == wxDefaultSize) {
			wxMemoryInputStream probe_stream(entry->png_data, entry->png_len);
			wxImage probe;
			if (probe.LoadFile(probe_stream, wxBITMAP_TYPE_PNG)) {
				sizeDef = wxSize(probe.GetWidth(), probe.GetHeight());
			}
		}
		if (sizeDef != wxDefaultSize) {
			// A malformed SVG, or one using features NanoSVG cannot render, yields a
			// non-ok bundle; fall through to the PNG raster path below rather than
			// returning nothing.
			wxBitmapBundle svg =
				wxBitmapBundle::FromSVG(entry->svg_data, entry->svg_len, sizeDef);
			if (svg.IsOk()) {
				return svg;
			}
		}
	}
#endif

	// No SVG twin (or wx built without NanoSVG support): serve the PNG at the requested logical
	// size plus a smooth 2x upscale, so DPI-aware widgets still get something better than
	// stretched 1x art. The mask is turned into an alpha channel first, because high-quality
	// scaling of a masked image smears the mask colour into the icon edges.
	wxMemoryInputStream stream(entry->png_data, entry->png_len);
	wxImage image;
	if (!image.LoadFile(stream, wxBITMAP_TYPE_PNG)) {
		return wxBitmapBundle();
	}
	if (!image.HasAlpha()) {
		image.InitAlpha();
	}
	// Target logical size: the caller's request, or the PNG's natural size when it does not
	// care. Derive both the 1x and 2x renditions from the original image so each is a single
	// high-quality resample -- building the 2x from an already rescaled 1x would resample
	// twice.
	const wxSize target = (size == wxDefaultSize) ? wxSize(image.GetWidth(), image.GetHeight()) : size;
	wxImage image1x = (target.GetWidth() == image.GetWidth() && target.GetHeight() == image.GetHeight())
				  ? image
				  : image.Scale(target.GetWidth(), target.GetHeight(), wxIMAGE_QUALITY_HIGH);
	wxImage image2x = image.Scale(target.GetWidth() * 2, target.GetHeight() * 2, wxIMAGE_QUALITY_HIGH);
	return wxBitmapBundle::FromBitmaps(wxBitmap(image1x), wxBitmap(image2x));
}
