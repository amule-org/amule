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

#ifndef FILELAUNCH_H
#define FILELAUNCH_H

class CKnownFile;
class CPath;
class wxWindow;

/**
 * Handing a file to the desktop: opening it, and showing it in the file
 * manager. Shared by the Downloads and Shared Files lists so the two cannot
 * drift; both are compiled per-executable, and nothing here branches on the
 * build variant.
 *
 * Availability is decided by whether the path exists on *this* host rather
 * than by which binary is running. The monolithic app is the core, so its
 * files are always local; amulegui drives a separate daemon and holds that
 * daemon's path (EC_TAG_KNOWNFILE_PATH), which resolves here only when the
 * two share a filesystem. Testing the path covers both, and also catches the
 * case the old build-variant gate missed: a local file that has since been
 * moved or deleted.
 *
 * The one thing it cannot distinguish is a remote daemon whose path happens
 * to exist locally as a *different* file. That is accepted: the alternative
 * refuses every legitimate shared-filesystem setup.
 */
namespace FileLaunch
{

/**
 * Full on-disk path of `file` -- the ".part" in the temp directory while it is
 * still downloading, the real name in its destination once complete.
 *
 * @return false when no path can be formed, which is normal in amulegui
 *         before the daemon has streamed one.
 */
bool ResolvePath(const CKnownFile *file, CPath &out);

/// True when the file can be handed to the desktop: its path resolves and exists.
bool CanOpen(const CKnownFile *file);

/**
 * True when the file's folder can be shown.
 *
 * Stricter than CanOpen: an incomplete download is a ".part" in the temp
 * directory, and revealing that is never what the user meant.
 */
bool CanReveal(const CKnownFile *file);

/**
 * Open the file with the desktop's handler for its type.
 *
 * Media goes to the configured video player when one is set -- including an
 * incomplete download, which is the preview case. Everything else goes to the
 * platform opener regardless of that setting, so "Open" on an archive does not
 * launch the video player.
 */
void Open(CKnownFile *file, wxWindow *parent);

/// Show the file's folder in the desktop's file manager.
void Reveal(const CKnownFile *file, wxWindow *parent);

} // namespace FileLaunch

#endif // FILELAUNCH_H
