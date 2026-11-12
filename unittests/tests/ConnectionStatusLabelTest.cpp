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
#include "ConnectionStatusLabel.h"

#include <wx/wx.h>
#include <wx/tooltip.h>

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
	wxDialog dialog(nullptr,
		wxID_ANY,
		"Connection status layout",
		wxDefaultPosition,
		wxDefaultSize,
		wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
	auto *log = new wxStaticText(&dialog,
		wxID_ANY,
		"2026-11-12T06:00:00.000+00:00 INFO  Connected to !EmuleServer v1.6 (no needo) "
		"(https://github.com/dagg3r/IEmuleServer/) with HighID",
		wxDefaultPosition,
		wxSize(140, -1),
		wxST_NO_AUTORESIZE | wxST_ELLIPSIZE_END);
	auto *users = new wxStaticText(&dialog, wxID_ANY, "Users: 5.32M | Files: 17.70M");
	auto *rates = new wxStaticText(&dialog, wxID_ANY, "Up: 0.5 KiB/s | Down: 0.0");
	auto *connection = new ConnectionStatusLabel(&dialog, wxID_ANY, "Not Connected ...");
	auto *following = new wxStaticText(&dialog, wxID_ANY, "*");
	auto *strip = new wxBoxSizer(wxHORIZONTAL);
	strip->Add(log, wxSizerFlags(1).FixedMinSize().Border(wxRIGHT, 5));
	strip->Add(users, wxSizerFlags().Border(wxRIGHT, 5));
	strip->Add(rates, wxSizerFlags().Border(wxRIGHT, 5));
	strip->Add(connection, wxSizerFlags().Border(wxRIGHT, 5));
	strip->Add(following);
	dialog.SetSizer(strip);
	dialog.SetClientSize(dialog.FromDIP(wxSize(1200, 100)));
	dialog.Show();
	wxTheApp->Yield();

	Check(connection->GetToolTip() && connection->GetToolTip()->GetTip() == "Not Connected ...",
		"Initial connection tooltip lost its full text");
	Check((connection->GetWindowStyleFlag() & wxST_ELLIPSIZE_END) != 0,
		"Connection label does not ellipsize clipped text");
	const wxString shortStatus = "eD2k: Short server | Kad: Off";
	const wxString longStatus = "eD2k: !EmuleServer v1.6 (no needo) "
				    "(https://github.com/dagg3r/IEmuleServer/) | Kad: Connected";
	for (bool largeFont : { false, true }) {
		if (largeFont) {
			connection->SetFont(connection->GetFont().Scaled(1.5));
			for (int frame = 0; frame < 10; ++frame) {
				wxMilliSleep(10);
				wxTheApp->Yield(true);
			}
		}
		for (const wxString &status : { shortStatus,
			     longStatus,
			     longStatus + " — another very long server description",
			     longStatus }) {
			connection->SetLabel(status);
			strip->Layout();
			Check(connection->GetLabel() == status,
				"Connection text was destructively truncated");
			Check(connection->GetToolTip()->GetTip() == status,
				"Tooltip did not follow the full status");
			Check(connection->GetSize().x <= connection->FromDIP(300),
				"A server name exceeded the connection field's width cap");
			Check(log->GetSize().x >= log->FromDIP(140),
				"A server name consumed the log reservation");
			Check(log->GetRect().GetRight() < users->GetRect().GetLeft(),
				"Log overlaps user counts");
			Check(connection->GetRect().GetRight() < following->GetRect().GetLeft(),
				"Connection text overlaps the following status icon");
			Check(following->GetRect().GetRight() < dialog.GetClientSize().x,
				"A long name pushed the last status field outside the window");
		}
		connection->SetLabel(longStatus);
		strip->Layout();
		const int cappedLogWidth = log->GetSize().x;
		Check(connection->GetTextExtent(longStatus).x > connection->GetSize().x,
			"Long-name fixture did not require ellipsis");
		connection->SetLabel(longStatus + " — even more text");
		strip->Layout();
		Check(log->GetSize().x == cappedLogWidth, "Additional server text kept shrinking the log");
		for (int width : { 1000, 1280, 1100 }) {
			dialog.SetClientSize(dialog.FromDIP(wxSize(width, 100)));
			dialog.Layout();
			Check(log->GetSize().x >= log->FromDIP(140), "Resizing lost the log reservation");
			Check(connection->GetSize().x <= connection->FromDIP(300),
				"Resizing lost the name cap");
		}
		dialog.SetClientSize(dialog.FromDIP(wxSize(1200, 100)));
		dialog.Layout();
		connection->SetLabel("Kad: Off");
		strip->Layout();
		const int shortWidth = connection->GetSize().x;
		std::cout << "Connection layout: long-log=" << cappedLogWidth
			  << " short-log=" << log->GetSize().x << " short-status=" << shortWidth
			  << " client=" << dialog.GetClientSize().x << '\n';
		Check(shortWidth < connection->FromDIP(300),
			"A short status retained the long-name reservation");
		Check(connection->GetToolTip()->GetTip() == "Kad: Off",
			"Disconnect retained a stale server tooltip");
		Check(log->GetSize().x > cappedLogWidth, "A short status did not release space to the log");
		connection->SetLabel("eD2k: Café && server | Kad: Connecting");
		strip->Layout();
		Check(connection->GetToolTip()->GetTip() == connection->GetLabel(),
			"Tooltip changed Unicode or ampersand text");
		connection->SetLabel(wxEmptyString);
		strip->Layout();
		Check(connection->GetLabel().empty(), "Empty network status retained stale text");
	}
}
} // namespace

int main(int argc, char **argv)
{
	return RunGuiTest(argc, argv, "No GUI display available; skipping connection status tests", [] {
		TestConnectionStatus();
		std::cout << "Connection status layout tests passed\n";
	});
}
