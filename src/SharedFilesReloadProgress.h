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

#ifndef SHAREDFILESRELOADPROGRESS_H
#define SHAREDFILESRELOADPROGRESS_H

#include <cstddef>

#include <wx/string.h>

class wxWindow;

// Declarations only, with the implementation in the .cpp. This header is
// included by several GUI translation units, and pulling amule.h (and the
// C++ standard headers behind it) in here put std::byte in scope ahead of the
// <windows.h> family that some of those TUs include further down -- rpcndr.h
// then declares ::byte and every later use is ambiguous. Keeping the heavy
// includes on the other side of the .cpp boundary removes that ordering trap
// for every caller rather than fixing it one include block at a time.

//! The one wording for reload progress, so every caller reports it identically.
wxString SharedFilesScannedMessage(std::size_t filesScanned);

/**
 * Run a shared-files reload behind a progress dialog.
 *
 * CSharedFileList::Reload() walks every configured share root on the calling
 * thread, costing roughly a stat per shared file. On a large or network-
 * mounted tree that is seconds to minutes, and the no-callback overload yields
 * nothing at all -- so every GUI action that triggered one used to freeze the
 * window with no repaint, no progress and no indication why. Passing a yield
 * callback keeps the walk on this thread but pumps the event loop every 256
 * files, so the window repaints and stays draggable.
 *
 * Shared by every GUI entry point that can trigger a walk -- the shared-files
 * Reload button, the preferences Apply, and the category add/rename/delete
 * paths -- so they all behave the same and the dialog exists once.
 *
 * **Pumping here is safe.** wxProgressDialog restricts its yield to UI events,
 * so it does not dispatch the queued socket events that carry EC requests
 * (they reach the main loop via CoreNotify_* and wxQueueEvent). A remote client
 * therefore cannot be answered from the transiently-empty share map that
 * FindSharedFiles builds through, which is the hazard that rules out pumping
 * the loop generally during a walk.
 *
 * **No cancel button, deliberately.** FindSharedFiles() clears m_Files_map
 * before it starts walking, so an aborted walk leaves a partially populated
 * share list with nothing to roll back to. PrefsUnifiedDlg's shared-directory
 * commit can offer cancel only because it keeps the previous directory list
 * and re-walks against it; none of these callers has such prior state. A
 * cancel here needs a real rollback story first.
 */
void ReloadSharedFilesWithProgress(wxWindow *parent);

/**
 * Run the walk only if one is owed, i.e. something just called
 * RequestReload(). Used by the category paths, where the reload is requested
 * deep in CPreferences (shared with the daemon, so it cannot raise a dialog
 * itself) and only when the change actually affects the shareset.
 */
void ReloadSharedFilesIfPending(wxWindow *parent);

#endif // SHAREDFILESRELOADPROGRESS_H
// File_checked_for_headers
