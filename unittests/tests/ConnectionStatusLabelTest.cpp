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

#include "ConnectionStatusLabel.h"
#include "GuiTestDiagnostics.h"

#include <wx/tooltip.h>
#include <wx/wx.h>

#include <iostream>

class ConnectionStatusLabelTestApp : public wxApp
{
public:
	bool OnInit() override { return true; }
};

wxIMPLEMENT_APP_NO_MAIN(ConnectionStatusLabelTestApp);

namespace
{
void TestConnectionStatus()
{
	const wxString longStatus = "eD2k: " + wxString('x', 200) + " | Kad: Connected";
	wxDialog dialog(nullptr, wxID_ANY, "Connection status");
	auto *log = new wxStaticText(
		&dialog, wxID_ANY, "Log", wxDefaultPosition, wxSize(140, -1), wxST_NO_AUTORESIZE);
	// Created with the long text, as a translated initial label would be.
	auto *connection = new ConnectionStatusLabel(&dialog, wxID_ANY, longStatus);
	auto *strip = new wxBoxSizer(wxHORIZONTAL);
	strip->Add(log, wxSizerFlags(1).FixedMinSize());
	strip->Add(connection);
	dialog.SetSizer(strip);
	dialog.SetClientSize(dialog.FromDIP(wxSize(900, 40)));
	dialog.Show();
	wxTheApp->Yield();
	const int cap = connection->FromDIP(300);
	Check(connection->GetSize().x <= cap, "A long initial label exceeded the cap");

	auto show = [&](const wxString &status) {
		connection->SetLabel(status);
		strip->Layout();
	};
	show("Kad: Off");
	const int shortLog = log->GetSize().x;
	Check(connection->GetSize().x < cap, "A short status kept the long-name width");
	show(longStatus);
	Check(connection->GetSize().x <= cap, "A long server name exceeded the cap");
	Check(log->GetSize().x < shortLog, "A long server name did not use its capped width");
	show("Kad: Off");
	Check(log->GetSize().x == shortLog, "A short status did not give the space back to the log");

	show(wxControl::EscapeMnemonics("eD2k: Tom & Jerry"));
	Check(connection->GetToolTip()->GetTip() == "eD2k: Tom & Jerry",
		"The tooltip does not show the status as plain text");
}
} // namespace

int main(int argc, char **argv)
{
	return RunGuiTest(argc, argv, "No GUI display available; skipping connection status tests", [] {
		TestConnectionStatus();
		std::cout << "Connection status label tests passed\n";
	});
}
