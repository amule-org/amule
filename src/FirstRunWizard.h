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

#ifndef FIRSTRUNWIZARD_H
#define FIRSTRUNWIZARD_H

// The first-run setup wizard is a GUI-only feature; it is never built
// into the headless daemon (AMULE_DAEMON, wxUSE_GUI=0).
#ifndef AMULE_DAEMON

class wxWindow;

namespace FirstRunWizard
{
// Result of running the wizard: tells the caller which network
// bootstrap files the user asked to download once the network stack is
// up. The wizard itself applies every preference it collects (nickname,
// bandwidth, networks, ports, UPnP, folders) and saves them, but the
// actual server.met / nodes.dat downloads depend on the server list and
// sockets that the caller initialises afterwards.
struct Result
{
	bool finished = false;          // false if the user cancelled
	bool downloadServerMet = false; // fetch eD2k server list (server.met)
	bool downloadNodesDat = false;  // fetch Kad bootstrap nodes (nodes.dat)
};

// Show the modal first-run wizard. `parent` may be NULL (the wizard is
// shown before the main window exists on a fresh install).
//
// `needServerMet` / `needNodesDat` seed the bootstrap page: they reflect
// whether each file is currently missing, so we don't offer to download
// something that is already present.
Result Run(wxWindow *parent, bool needServerMet, bool needNodesDat);
} // namespace FirstRunWizard

#endif // !AMULE_DAEMON

#endif // FIRSTRUNWIZARD_H
