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
	auto *duration = dynamic_cast<wxSpinCtrl *>(wxWindow::FindWindowByName(_("Minimum duration"), panel));
	auto *bitrate =
		dynamic_cast<wxSpinCtrl *>(wxWindow::FindWindowByName(_("Minimum bitrate (kbps)"), panel));
	auto *unit = dynamic_cast<wxChoice *>(wxWindow::FindWindowByName(_("Duration unit"), panel));
	Check(duration && bitrate && unit, "Missing numeric metadata controls or duration unit");
	duration->SetValue(62);
	bitrate->SetValue(320);
	panel->Read(value);
	Check(value.minLength == 3720 && value.minBitrateKbps == 320 &&
			value.artist == wxString::FromUTF8("演奏者") && value.album == "Test Album" &&
			value.title == "OR AND NOT" && value.codec == "FLAC",
		"Fields did not reach search parameters");
	Check(panel->HasValues(), "Entered fields do not enable Reset");
	value.minLength = 61;
	panel->Set(value);
	CSearchList::CSearchParams restored;
	panel->Read(restored);
	Check(restored.minLength == 61 && restored.artist == value.artist && restored.title == value.title &&
			restored.album == value.album && restored.codec == value.codec &&
			restored.minBitrateKbps == value.minBitrateKbps,
		"Loading a recorded form lost seconds or media filters");
	value.minLength = 86400;
	panel->Set(value);
	panel->Read(restored);
	Check(restored.minLength == 86400, "Maximum duration was truncated");
	panel->Set(CSearchList::CSearchParams());
	Check(!panel->HasValues(), "Loading defaults retained the previous media filters");
	value.minLength = 61;
	value.minBitrateKbps = 320;
	panel->Set(value);
	panel->SetSupported(false);
	panel->Read(restored);
	Check(!panel->HasValues() && restored.minLength == 0 && restored.minBitrateKbps == 0 &&
			!duration->IsEnabled() && !bitrate->IsEnabled() && !unit->IsEnabled(),
		"An older daemon retained unsupported metadata filters");
	for (const auto &name : { _("Artist"), _("Album"), _("Title"), _("Codec") })
		Check(!Input(*panel, name)->IsEnabled(), "Unsupported text filter remained editable");
	panel->SetSupported(true);
	duration->SetValue(62);
	panel->Read(restored);
	Check(restored.minLength == 3720 && duration->IsEnabled() && bitrate->IsEnabled(),
		"Reconnect did not restore the default minutes editor");
	Check(unit->IsEnabled() && unit->GetSelection() == 0,
		"Reconnect did not enable the default duration unit");
	for (const auto &name : { _("Artist"), _("Album"), _("Title"), _("Codec") })
		Check(Input(*panel, name)->IsEnabled(), "Reconnect left a text filter disabled");
	panel->Reset();
	panel->Read(value);
	Check(!panel->HasValues() && value.minBitrateKbps == 0 && value.minLength == 0 &&
			value.artist.empty() && value.album.empty() && value.title.empty() &&
			value.codec.empty(),
		"Reset did not clear all six fields");
}

void TestDurationBoundariesAndUnitChanges()
{
	wxDialog dialog(nullptr, wxID_ANY, "Recorded media duration");
	CSearchMetadataPanel panel(&dialog);
	auto *duration =
		dynamic_cast<wxSpinCtrl *>(wxWindow::FindWindowByName(_("Minimum duration"), &panel));
	auto *unit = dynamic_cast<wxChoice *>(wxWindow::FindWindowByName(_("Duration unit"), &panel));
	Check(duration && unit, "Missing duration controls");
	struct Case
	{
		uint32_t seconds;
		int number;
		int unit;
	};
	const Case cases[] = { { 0, 0, 0 },
		{ 1, 1, 1 },
		{ 59, 59, 1 },
		{ 60, 1, 0 },
		{ 61, 61, 1 },
		{ 3599, 3599, 1 },
		{ 3600, 60, 0 },
		{ 86399, 86399, 1 },
		{ 86400, 1440, 0 } };
	for (const auto &test : cases) {
		CSearchList::CSearchParams recorded;
		recorded.minLength = test.seconds;
		recorded.minBitrateKbps = 1000000;
		panel.Set(recorded);
		CSearchList::CSearchParams restored;
		panel.Read(restored);
		Check(restored.minLength == test.seconds && restored.minBitrateKbps == 1000000,
			"A boundary duration or bitrate changed during restoration");
		Check(duration->GetValue() == test.number && unit->GetSelection() == test.unit,
			"Restored duration uses the wrong number or unit");
		Check(duration->GetMax() == (test.unit == 0 ? 1440 : 86400),
			"Duration range does not match its selected unit");
	}
	panel.Reset();
	const auto selectUnit = [&unit](int selection) {
		unit->SetSelection(selection);
		wxCommandEvent changed(wxEVT_CHOICE, unit->GetId());
		changed.SetEventObject(unit);
		changed.SetInt(selection);
		unit->GetEventHandler()->ProcessEvent(changed);
	};
	selectUnit(1);
	Check(duration->GetMax() == 86400, "Choosing seconds did not expand the duration range");
	duration->SetValue(61);
	CSearchList::CSearchParams restored;
	panel.Read(restored);
	Check(restored.minLength == 61, "The seconds editor still interpreted input as minutes");
	selectUnit(0);
	Check(duration->GetMax() == 1440, "Choosing minutes did not restore the duration range");
	panel.Read(restored);
	Check(restored.minLength == 3660, "The minutes editor still interpreted input as seconds");
	selectUnit(1);
	panel.Reset();
	Check(unit->GetSelection() == 0 && duration->GetMax() == 1440 && !panel.HasValues(),
		"Reset retained seconds mode or a previous filter");
}
} // namespace

int main(int argc, char **argv)
{
	return RunGuiTest(argc, argv, nullptr, [] {
		TestFields();
		TestDurationBoundariesAndUnitChanges();
		std::cout << "PASS: metadata fields, duration boundaries, unit changes, capability changes "
			     "and reset\n";
	});
}
