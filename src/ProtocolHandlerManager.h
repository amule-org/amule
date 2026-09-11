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

#include <wx/arrstr.h>
#include <wx/string.h>

// Cross-platform "aMule is the default handler for X" toggle, where X is an
// ed2k:// / magnet: link or an .emulecollection file. Mirrors AutostartManager's
// shape: the OS is the source of truth, per-user, no elevation. The per-OS store
// is:
//
//   Windows : HKCU\Software\Classes\<scheme> (URL-protocol keys, DefaultIcon +
//             shell\open\command pointing at amule.exe). The collection file
//             type instead uses the ProgID layout every Windows file association
//             needs: the extension key names a ProgID, and the ProgID carries
//             the icon and the open command.
//   Linux   : $XDG_CONFIG_HOME/mimeapps.list [Default Applications],
//             x-scheme-handler/<scheme>=org.amule.aMule.desktop or
//             application/x-emule-collection=... for the file type. The
//             .desktop still declares MimeType= as the advertise layer.
//   macOS   : LaunchServices -- LSSetDefaultHandlerForURLScheme for the schemes,
//             LSSetDefaultRoleHandlerForContentType for the collection UTI. The
//             .app's Info.plist declares CFBundleURLTypes /
//             CFBundleDocumentTypes so LaunchServices considers us eligible.
//
// What "enable" can promise for the file type: from Windows 8 on, the user's
// chosen default lives in a hash-protected UserChoice key no application may
// write, so enabling adds aMule to the "Open with" list and takes the default
// only when nothing else has claimed the extension. macOS and Linux have no such
// restriction.
//
// magnet handling keeps its ed2k-compatibility requirement: magnet URIs must
// carry xt=urn:ed2k:/urn:ed2khash: + xl=, enforced by CMagnetED2KConverter.
// This class only owns the OS registration; parsing lives in
// CamuleAppCommon::CheckPassedLink() and in CMuleCollection.

// What aMule can register itself as the handler for. Named for the
// registration, not the payload: the two schemes and the collection file
// type share one policy layer and differ only in the per-OS key they
// write.
enum class HandlerTarget
{
	Ed2kScheme,
	MagnetScheme,
	CollectionFile,
};

class ProtocolHandlerManager
{
public:
	// Returns true if the OS's per-user default handler for `scheme`
	// is aMule. Doesn't validate the registered path against the
	// running binary (use SelfHealOnStartup() for that).
	static bool IsEnabled(HandlerTarget scheme);

	// Writes or overwrites the OS handler entry for `scheme` to point at the
	// running binary's canonical path. Idempotent. Does NOT prompt before
	// overwriting a pre-existing third-party handler -- the caller owns the
	// "already registered to another app, overwrite?" UX, via GetCurrentHandler().
	static bool Enable(HandlerTarget scheme);

	// Removes the OS handler entry for `scheme` if aMule is the current
	// handler. Idempotent (no-op if we aren't the current handler);
	// returns true on success. Never wipes a third-party handler.
	static bool Disable(HandlerTarget scheme);

	// A short human-readable name of the app currently registered as the default
	// handler for `scheme` ("Transmission" on macOS, an executable path on
	// Windows/Linux), or empty when no handler is set or aMule is it. Used to build
	// the "another app is currently the default, overwrite?" confirm dialog, never
	// to gate Enable() -- see its contract note.
	static wxString GetCurrentHandler(HandlerTarget scheme);

	// Called once from CamuleApp::OnInit. For each scheme where aMule is the
	// current default handler AND the registered path differs from the canonical
	// path of the running binary, rewrites the entry so the next click launches the
	// right binary -- the "user moved the AppImage / .app / install dir" case.
	// Does nothing where we are not the current handler: disabling is a deliberate
	// user choice.
	static void SelfHealOnStartup();

	// Resolves argv[0] to its canonical absolute path (realpath() on POSIX,
	// GetModuleFileNameW() on Windows). Duplicates the AutostartManager helper
	// rather than depending on it, so this class stays self-contained.
	static wxString GetCanonicalExecutablePath();
};

// Queues already-validated eD2k links into the ED2KLinks file, so
// CDownloadQueue::Process (or the equivalent polling loop in
// CamuleRemoteGuiApp) picks them up on its next 1-second tick. Build-agnostic:
// monolithic amule, amulegui and amuled each have a polling loop that calls
// AddLinksFromFile on the same file. Safe to call before the download queue is
// wired, the cold-launch case: this only writes.
//
// The whole batch costs one open/write, and raises the window once.
void ProtocolHandler_QueueLinks(const wxArrayString &links);

// Queues a scheme-clicked URL (ed2k:// or magnet:), percent-decoding it
// first for URLs delivered by browsers, then handing off to
// ProtocolHandler_QueueLinks.
void ProtocolHandler_QueueSchemeLink(const wxString &url);

#endif // PROTOCOLHANDLERMANAGER_H
// File_checked_for_headers
