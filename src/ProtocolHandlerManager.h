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

#ifndef PROTOCOLHANDLERMANAGER_H
#define PROTOCOLHANDLERMANAGER_H

#include <wx/string.h>

// Cross-platform "aMule is the default handler for ed2k:// / magnet:
// links" toggle. Mirrors AutostartManager's shape: the OS is the
// source of truth, per-user, no elevation. The per-OS store is:
//
//   Windows : HKCU\Software\Classes\<scheme> (URL-protocol keys,
//             DefaultIcon + shell\open\command pointing at amule.exe)
//   Linux   : $XDG_CONFIG_HOME/mimeapps.list [Default Applications]
//             x-scheme-handler/<scheme>=org.amule.aMule.desktop
//             (also written via `xdg-mime default` when available;
//             the .desktop still declares MimeType= as the "advertise"
//             layer so file managers pick us up even without a default)
//   macOS   : LaunchServices — LSSetDefaultHandlerForURLScheme(kCFBundle,
//             CFSTR("ed2k")/CFSTR("magnet")). The .app's Info.plist
//             declares CFBundleURLTypes so LaunchServices considers us
//             eligible in the first place.
//
// magnet handling in aMule keeps its ed2k-compatibility requirement:
// magnet URIs must carry xt=urn:ed2k:/urn:ed2khash: + xl= (enforced by
// CMagnetED2KConverter in MagnetURI.cpp). BitTorrent-only magnets are
// out of scope. This class only owns the OS registration; parsing lives
// in CamuleAppCommon::CheckPassedLink() which already handles both
// schemes.

enum class UriScheme
{
	Ed2k,
	Magnet,
};

class ProtocolHandlerManager
{
public:
	// Returns true if the OS's per-user default handler for `scheme`
	// is aMule. Doesn't validate the registered path against the
	// running binary (use SelfHealOnStartup() for that).
	static bool IsEnabled(UriScheme scheme);

	// Writes/overwrites the OS handler entry for `scheme` to point at
	// the running binary's canonical path. Idempotent; returns true on
	// success. Does NOT prompt the user before overwriting a
	// pre-existing third-party handler — the caller (prefs toggle,
	// wizard, CLI, installer) is responsible for the "already registered
	// to another app, overwrite?" UX. Use GetCurrentHandler() to check.
	static bool Enable(UriScheme scheme);

	// Removes the OS handler entry for `scheme` if aMule is the current
	// handler. Idempotent (no-op if we aren't the current handler);
	// returns true on success. Never wipes a third-party handler.
	static bool Disable(UriScheme scheme);

	// Returns a short human-readable name of the app currently
	// registered as the default handler for `scheme` (e.g. "Transmission"
	// on macOS, executable path on Windows/Linux), or empty string if
	// no handler is set or the current handler is aMule. Used by the
	// prefs toggle to build the "another app is currently the default,
	// overwrite?" confirm dialog.
	//
	// Never used to gate Enable() — see the Enable() contract note.
	static wxString GetCurrentHandler(UriScheme scheme);

	// Called once from CamuleApp::OnInit. For each scheme where aMule is
	// currently the default handler AND the registered path differs
	// from the canonical path of the running binary, rewrites the entry
	// so the next click launches the right binary. Handles the
	// "user moved the AppImage / .app / install dir" case.
	//
	// Does nothing if we aren't the current handler for a scheme —
	// disabling is always a deliberate user choice we don't second-
	// guess.
	static void SelfHealOnStartup();

	// Resolves argv[0] to its canonical absolute path (realpath() on
	// POSIX, GetModuleFileNameW() on Windows). Duplicates the helper
	// in AutostartManager rather than depending on it so this class
	// stays self-contained; the resolution is a couple of lines and
	// both callers want the same thing.
	static wxString GetCanonicalExecutablePath();
};

// Queues a scheme-clicked URL (ed2k:// or magnet:) into the ED2KLinks
// file so CDownloadQueue::Process (or the equivalent polling loop in
// CamuleRemoteGuiApp) picks it up on its next 1-second tick and forwards
// to AddLink. Handles percent-decoding for URLs delivered by browsers.
// Build-agnostic: works from monolithic amule, remote-GUI amulegui, or
// daemon amuled — each has its own theApp with an ED2KLinks polling
// loop that ends up calling AddLinksFromFile on the same file we write
// to here. Safe to call before the download queue is fully wired
// (cold-launch case): we just write, we don't try to enqueue.
void ProtocolHandler_QueueSchemeLink(const wxString &url);

#endif // PROTOCOLHANDLERMANAGER_H
// File_checked_for_headers
