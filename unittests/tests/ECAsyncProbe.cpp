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

// Exercise amulegui's event-driven CRemoteConnect without a display server.
#include "RemoteConnect.h"
#include "GuiEvents.h"
#include <common/MD5Sum.h>
#include <wx/init.h>
#include <wx/app.h>
#include <chrono>
#include <mutex>
#include <thread>
#include <iostream>

class ProbeApp : public wxAppConsole
{
};
wxIMPLEMENT_APP_NO_MAIN(ProbeApp);

namespace
{
std::mutex mutex;
std::vector<std::unique_ptr<MuleNotify::CMuleNotiferBase>> pending;
} // namespace
namespace MuleNotify
{
void HandleNotificationAlways(const CMuleNotiferBase &event)
{
	std::lock_guard<std::mutex> lock(mutex);
	pending.emplace_back(event.Clone());
}
void HandleNotification(const CMuleNotiferBase &event)
{
	HandleNotificationAlways(event);
}
} // namespace MuleNotify
int main(int argc, char **argv)
{
	if (argc != 2 && argc != 3)
		return 2;
	wxInitializer init;
	if (!init.IsOk())
		return 2;
	wxEvtHandler notifier;
	bool done = false, connected = false;
	notifier.Bind(wxEVT_EC_CONNECTION, [&](wxEvent &event) {
		done = true;
		connected = static_cast<wxECSocketEvent &>(event).GetResult();
	});
	CRemoteConnect connection(&notifier);
	connection.SetCapabilities(false, false, false);
	CAsioService service;
	if (!connection.ConnectToCore("127.0.0.1",
		    std::stoi(argv[1]),
		    MD5Sum(argc == 3 ? wxString::FromUTF8(argv[2]) : wxString("regression")).GetHash(),
		    "amulegui async regression",
		    "1"))
		return 1;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	while (!done && std::chrono::steady_clock::now() < deadline) {
		// UI locality queries must not replace the pending SRP exponent or proof.
		connection.IsConnectedToLocalHost();
		std::vector<std::unique_ptr<MuleNotify::CMuleNotiferBase>> events;
		{
			std::lock_guard<std::mutex> lock(mutex);
			events.swap(pending);
		}
		for (const auto &event : events)
			event->Notify();
		wxTheApp->ProcessPendingEvents();
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	connected = done && connected && connection.IsAEADNegotiated();
	std::cout << connection.GetServerReply() << '\n';
	connection.CloseSocket();
	service.Stop();
	return connected ? 0 : 1;
}
