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
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA
//

#include "GuiTestDiagnostics.h"
#include "SearchMetadataPanel.h"
#include <wx/spinctrl.h>
#include <wx/wx.h>
#include <wx/evtloop.h>
#include <iostream>
#include <stdexcept>

class SearchMetadataTestApp : public wxApp
{
public:
	bool OnInit() override { return true; }
};
wxIMPLEMENT_APP_NO_MAIN(SearchMetadataTestApp);

namespace
{
void Check(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}
wxTextCtrl *Input(CSearchMetadataPanel &panel, const wxString &name)
{
	auto *field = dynamic_cast<wxTextCtrl *>(wxWindow::FindWindowByName(name, &panel));
	Check(field != nullptr, "Missing accessible metadata input");
	return field;
}
void TestFields()
{
	wxEventLoop loop;
	wxEventLoopActivator active(&loop);
	wxDialog dialog(nullptr,
		wxID_ANY,
		"Advanced media search",
		wxDefaultPosition,
		wxDefaultSize,
		wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
	auto *panel = new CSearchMetadataPanel(&dialog);
	auto *layout = new wxBoxSizer(wxVERTICAL);
	layout->Add(panel, wxSizerFlags(1).Expand().Border(wxALL, 12));
	dialog.SetSizerAndFit(layout);
	dialog.SetSize(dialog.FromDIP(wxSize(850, 250)));
	dialog.Show();
	wxYield();
	CSearchList::CSearchParams value;
	Input(*panel, _("Artist"))->SetValue(wxString::FromUTF8("演奏者"));
	Input(*panel, _("Album"))->SetValue("Test Album");
	Input(*panel, _("Title"))->SetValue("OR AND NOT");
	Input(*panel, _("Codec"))->SetValue("FLAC");
	auto *duration = dynamic_cast<wxSpinCtrl *>(
		wxWindow::FindWindowByName(_("Minimum duration (minutes)"), panel));
	auto *bitrate =
		dynamic_cast<wxSpinCtrl *>(wxWindow::FindWindowByName(_("Minimum bitrate (kbps)"), panel));
	Check(duration && bitrate, "Missing numeric metadata controls");
	duration->SetValue(62);
	bitrate->SetValue(320);
	panel->Read(value);
	Check(value.minLength == 3720 && value.minBitrateKbps == 320 &&
			value.artist == wxString::FromUTF8("演奏者") && value.album == "Test Album" &&
			value.title == "OR AND NOT" && value.codec == "FLAC",
		"Fields did not reach search parameters");
	Check(panel->HasValues(), "Entered fields do not enable Reset");
	panel->Reset();
	panel->Read(value);
	Check(!panel->HasValues() && value.minBitrateKbps == 0 && value.minLength == 0 &&
			value.artist.empty() && value.album.empty() && value.title.empty() &&
			value.codec.empty(),
		"Reset did not clear all six fields");
}
} // namespace

int main(int argc, char **argv)
{
	InstallGuiTestDiagnostics();
	if (!wxEntryStart(argc, argv))
		return 77;
	RestoreGuiTestLog();
	int result = 0;
	if (!wxTheApp->CallOnInit())
		result = 1;
	else {
		try {
			TestFields();
			std::cout << "PASS: metadata fields and reset\n";
		} catch (const std::exception &error) {
			std::cerr << error.what() << '\n';
			result = 1;
		}
		wxTheApp->OnExit();
	}
	wxEntryCleanup();
	return result;
}
