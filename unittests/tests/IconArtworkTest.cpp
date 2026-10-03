// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 aMule Team
// GUI rendering regression test. Requires a display; otherwise CTest skips it.

#include <wx/app.h>
#include <wx/image.h>
#include <wx/settings.h>
#include <wx/menu.h>
#include <wx/filename.h>
#include "CamuleArtProvider.h"
#include "CountryFlags.h"
#include "MenuIcons.h"
#include "icons/icon_data.h"
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <cstdlib>

class CheckApp : public wxApp
{
public:
	bool OnInit() override { return true; }
};
wxIMPLEMENT_APP_NO_MAIN(CheckApp);
void require(bool ok, const char *message)
{
	if (!ok)
		throw std::runtime_error(message);
}
int main(int argc, char **argv)
{
	if (!wxEntryStart(argc, argv))
		return 77;
	if (!wxTheApp->CallOnInit()) {
		wxEntryCleanup();
		return 1;
	}
	wxInitAllImageHandlers();
	wxArtProvider::Push(new CamuleArtProvider);
	int count = 0, flags = 0, menus = 0;
	const auto entries = amule_get_all_icons(&count);
	const char *output = std::getenv("AMULE_ICON_TEST_OUTPUT");
	if (output)
		wxFileName::Mkdir(wxString::FromUTF8(output), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
	int status = 0;
	try {
		CCountryFlags cache;
		for (int i = 0; i < count; ++i) {
			const auto &entry = entries[i];
			bool flag = std::strncmp(entry.name, "flag_", 5) == 0;
			bool menu = std::strncmp(entry.name, "menu_", 5) == 0;
			if (!flag && !menu)
				continue;
			if (flag)
				++flags;
			else
				++menus;
			for (double scale : { 1.0, 1.5, 2.0 }) {
				wxBitmap b;
				// wxMSW scales DIP into drawing coordinates before GetFlag;
				// its logical bitmap dimensions are physical pixels. GTK/macOS
				// instead use a backing scale with fixed logical dimensions.
#ifdef __WXMSW__
				const wxSize flagSize(wxRound(16 * scale), wxRound(12 * scale));
				const double contentScale = 1.0;
#else
				const wxSize flagSize(16, 12);
				const double contentScale = scale;
#endif
				if (flag)
					b = cache.GetFlag(
						wxString::FromUTF8(entry.name + 5), flagSize, contentScale);
				else {
					const auto bundle = wxArtProvider::GetBitmapBundle(
						wxString("amule:") + entry.name, wxART_MENU, wxSize(16, 16));
					require(bundle.IsOk(), "menu bundle invalid");
					b = bundle.GetBitmap(
						wxSize(wxRound(16 * scale), wxRound(16 * scale)));
				}
				require(b.IsOk(), "bitmap invalid");
				require(b.GetWidth() == wxRound(16 * scale), "wrong pixel width");
				if (flag)
					require(b.GetLogicalSize() == flagSize, "wrong logical size");
				auto image = b.ConvertToImage();
				if (output)
					require(image.SaveFile(wxFileName(wxString::FromUTF8(output),
								       wxString::Format("%s-%d.png",
									       entry.name,
									       wxRound(16 * scale)))
								       .GetFullPath(),
							wxBITMAP_TYPE_PNG),
						"PNG export failed");
			}
		}
		const auto unknown = cache.GetFlag("unknown", wxSize(16, 12), 1);
		const auto missing = cache.GetFlag("zz", wxSize(16, 12), 1);
		require(std::memcmp(unknown.ConvertToImage().GetData(),
				missing.ConvertToImage().GetData(),
				16 * 12 * 3) == 0,
			"unknown fallback changed");
		// Windows converts DIP to drawing coordinates; backing scale remains one.
		auto windows = cache.GetFlag("us", wxSize(32, 24), 1);
		require(windows.GetLogicalSize() == wxSize(32, 24), "Windows DIP sizing");
		// The same bundle must render correctly after a monitor transition back to 1x.
		auto back = cache.GetFlag("us", wxSize(16, 12), 1);
		require(back.GetSize() == wxSize(16, 12), "monitor transition sizing");
		wxMenu menu;
		auto item = AppendMenuIcon(&menu, 1234, "&Pause", MenuIcon::Pause);
		require(item->GetId() == 1234 && item->GetItemLabel() == "&Pause",
			"menu command or mnemonic changed");
		require(item->GetBitmapBundle().IsOk(), "menu bitmap missing");
		item->Enable(false);
		require(!item->IsEnabled(), "disabled state");
		auto check = menu.AppendCheckItem(1235, "Auto");
		check->Check();
		require(check->IsChecked(), "check state");
		const auto textColour = wxSystemSettings::GetColour(wxSYS_COLOUR_MENUTEXT);
		const auto pause = item->GetBitmapBundle().GetBitmap(wxSize(16, 16)).ConvertToImage();
		require(pause.GetRed(5, 7) == textColour.Red() &&
				pause.GetGreen(5, 7) == textColour.Green() &&
				pause.GetBlue(5, 7) == textColour.Blue(),
			"menu theme colour");
	} catch (const std::exception &e) {
		std::cerr << e.what() << "\n";
		status = 1;
	}
	wxArtProvider::Pop();
	wxTheApp->OnExit();
	wxEntryCleanup();
	if (!status)
		std::cout << "PASS: " << flags << " flags and " << menus
			  << " menu icons at 1x/1.5x/2x; fallback, DPI transition, menu semantics and theme "
			     "colour\n";
	return status;
}
