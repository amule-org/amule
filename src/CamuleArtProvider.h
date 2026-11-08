//
// CamuleArtProvider -- wxArtProvider subclass exposing aMule's bundled
// icons through wxArtProvider::GetBitmap() / GetIcon() / GetBitmapBundle().
//
// The icon data itself lives in src/icons/icon_data.c (build-time
// generated from src/icons/ by src/icons/embed_icons.py).
// CamuleArtProvider just glues that lookup table to the wx art-id
// system: art ids of the form "amule:<name>" map to the entry of the
// same name.  Examples: "amule:amule", "amule:sort_dn", "amule:menu_pause_fill".
// An icon with an SVG twin is rendered from it on every path; the PNG is
// the fallback for icons without one.
//
// Push one instance during app init (CamuleApp::OnInit and
// CamuleRemoteGuiApp::OnInit do this) so every later
// `wxArtProvider::GetBitmap("amule:<name>", ...)` resolves through it.
//

#ifndef SRC_CAMULE_ART_PROVIDER_H
#define SRC_CAMULE_ART_PROVIDER_H

#include <wx/artprov.h>
#include <wx/bmpbndl.h> // Needed for wxBitmapBundle

class CamuleArtProvider : public wxArtProvider
{
public:
	// Prefix every aMule art id starts with -- e.g. "amule:sort_dn".
	static const wxString PREFIX;

	// Build a full art id from a short icon name.  E.g.
	// CamuleArtProvider::MakeId("sort_dn") -> "amule:sort_dn".
	static wxString MakeId(const wxString &name) { return PREFIX + name; }

	// Menu colour is applied outside wxArtProvider's global bundle cache.
	static wxBitmapBundle GetMenuBitmapBundle(
		const wxString &name, const wxSize &size, const wxColour &colour, bool preferSvg = true);

protected:
	wxBitmap CreateBitmap(const wxArtID &id, const wxArtClient &client, const wxSize &size) override;

	// Bundle-aware lookup behind wxArtProvider::GetBitmapBundle(). PNG-only icons serve their
	// embedded 1x/2x/3x PNGs, resampled with high quality for sizes in between.
	wxBitmapBundle CreateBitmapBundle(
		const wxArtID &id, const wxArtClient &client, const wxSize &size) override;
};

#endif // SRC_CAMULE_ART_PROVIDER_H
