// CamuleArtProvider -- see CamuleArtProvider.h for the contract.

#include "CamuleArtProvider.h"
#include "IconDensities.h"
#include "TrayIconBundle.h"

#include "icons/icon_data.h"

#include <array>

#include <wx/bitmap.h>
#include <wx/bmpbndl.h> // Needed for wxBitmapBundle::FromSVG / FromImpl
#include <wx/image.h>
#include <wx/mstream.h>
#include <wx/settings.h>

const wxString CamuleArtProvider::PREFIX = "amule:";

namespace
{
const struct AMuleIconEntry *FindIcon(const wxArtID &id)
{
	// Only resolve our own art ids; let other providers handle the rest.
	if (!id.StartsWith(CamuleArtProvider::PREFIX)) {
		return nullptr;
	}
	return amule_find_icon(id.Mid(CamuleArtProvider::PREFIX.length()).utf8_str().data());
}

bool LoadPng(const struct AMuleIconEntry *entry, wxImage &image)
{
	wxMemoryInputStream stream(entry->png_data, entry->png_len);
	return image.LoadFile(stream, wxBITMAP_TYPE_PNG);
}

// The embedded PNG densities, resampled with high quality for sizes in between.
class PngDensityBundle : public wxBitmapBundleImpl
{
public:
	PngDensityBundle(const std::array<wxImage, 3> &images, const wxSize &size)
	: m_images(images)
	, m_size(size)
	{
	}

	wxSize GetDefaultSize() const override { return m_size; }

	wxSize GetPreferredBitmapSizeAtScale(double scale) const override
	{
		return wxSize(wxRound(m_size.x * scale), wxRound(m_size.y * scale));
	}

	wxBitmap GetBitmap(const wxSize &size) override
	{
		if (!m_last.IsOk() || m_last.GetSize() != size) {
			const wxImage image = ScaleIconDensity(m_images, size);
			m_last = image.IsOk() ? wxBitmap(image) : wxBitmap();
		}
		return m_last;
	}

private:
	const std::array<wxImage, 3> m_images;
	const wxSize m_size;
	wxBitmap m_last;
};

// The icon's SVG twin as a bundle, or an invalid bundle when there is none or it does not parse.
// wxDefaultSize takes the PNG twin's natural size, which FromSVG needs as its default.
wxBitmapBundle SvgBundle(
	const struct AMuleIconEntry *entry, const wxSize &size, const wxColour *colour = nullptr)
{
#ifdef wxHAS_SVG
	if (entry->svg_data != nullptr && entry->svg_len > 0) {
		wxSize sizeDef(size);
		wxImage probe;
		if (sizeDef == wxDefaultSize && LoadPng(entry, probe)) {
			sizeDef = probe.GetSize();
		}
		if (sizeDef != wxDefaultSize) {
			if (colour) {
				wxString svg = wxString::FromUTF8(
					reinterpret_cast<const char *>(entry->svg_data), entry->svg_len);
				svg.Replace("#212529", colour->GetAsString(wxC2S_HTML_SYNTAX));
				return wxBitmapBundle::FromSVG(svg.utf8_str(), sizeDef);
			}
			return wxBitmapBundle::FromSVG(entry->svg_data, entry->svg_len, sizeDef);
		}
	}
#else
	wxUnusedVar(entry);
	wxUnusedVar(size);
	wxUnusedVar(colour);
#endif
	return wxBitmapBundle();
}
wxBitmapBundle IconBundle(const struct AMuleIconEntry *entry,
	const wxSize &size,
	const wxColour *colour = nullptr,
	bool preferSvg = true)
{
	// A malformed SVG, or one using features NanoSVG cannot render, yields a non-ok bundle;
	// fall through to the PNG raster path below rather than returning nothing.
	wxBitmapBundle svg = preferSvg ? SvgBundle(entry, size, colour) : wxBitmapBundle();
	if (svg.IsOk()) {
		return svg;
	}

	// No SVG twin (or wx built without NanoSVG support): serve the PNG densities.
	auto images = LoadIconDensities(*entry);
	if (!images[0].IsOk()) {
		return wxBitmapBundle();
	}
	if (colour) {
		// Monochrome PNG fallbacks retain their alpha, including antialiased edges.
		for (auto &image : images) {
			if (image.IsOk()) {
				image.SetRGB(wxRect(image.GetSize()),
					colour->Red(),
					colour->Green(),
					colour->Blue());
			}
		}
	}
	if (wxString::FromUTF8(entry->name).StartsWith("tray_")) {
		return MuleTray::MakeArtworkFallback(images[0], size);
	}
	return wxBitmapBundle::FromImpl(
		new PngDensityBundle(images, size == wxDefaultSize ? images[0].GetSize() : size));
}
} // namespace

wxBitmap CamuleArtProvider::CreateBitmap(
	const wxArtID &id, const wxArtClient &WXUNUSED(client), const wxSize &size)
{
	const struct AMuleIconEntry *entry = FindIcon(id);
	if (entry == nullptr) {
		return wxNullBitmap;
	}

	// Rendered from the SVG twin at the requested pixel size when there is one, so raster
	// consumers (image lists, DC drawing) get the same artwork as bundle consumers.
	const wxBitmapBundle svg = SvgBundle(entry, size);
	if (svg.IsOk()) {
		return svg.GetBitmap(size == wxDefaultSize ? svg.GetDefaultSize() : size);
	}

	// GetBitmap() passes wxDefaultSize when the caller does not care: the 1x PNG's size wins.
	const auto images = LoadIconDensities(*entry);
	if (!images[0].IsOk()) {
		return wxNullBitmap;
	}
	const wxImage image = ScaleIconDensity(images, size == wxDefaultSize ? images[0].GetSize() : size);
	return image.IsOk() ? wxBitmap(image) : wxNullBitmap;
}

wxBitmapBundle CamuleArtProvider::CreateBitmapBundle(
	const wxArtID &id, const wxArtClient &WXUNUSED(client), const wxSize &size)
{
	const struct AMuleIconEntry *entry = FindIcon(id);
	if (entry == nullptr) {
		return wxBitmapBundle();
	}

	return IconBundle(entry, size);
}

wxBitmapBundle CamuleArtProvider::GetMenuBitmapBundle(
	const wxString &name, const wxSize &size, const wxColour &colour, bool preferSvg)
{
	if (!name.StartsWith("menu_")) {
		return wxBitmapBundle();
	}
	const auto *entry = amule_find_icon(name.utf8_str());
	// Colour-dependent artwork must not enter wxArtProvider's global cache.
	// A new popup always uses the current theme, even on wx versions whose
	// Push/Pop cache clear leaves bitmap bundles cached.
	return entry ? IconBundle(entry, size, &colour, preferSvg) : wxBitmapBundle();
}
