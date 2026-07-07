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
// Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
//

#ifndef APPIMAGEENV_H
#define APPIMAGEENV_H

#include <wx/utils.h> // wxExecuteEnv

namespace AppImageEnv
{

// Fill `env` with a copy of the current environment that is safe for launching
// an external (non-bundled) program -- the preview player, the configured web
// browser, a user-event command, etc. When running inside an AppImage, every
// search-path entry that points into the bundle ($APPDIR) is stripped so the
// child loads the host's system libraries and modules instead of the older
// bundled copies; otherwise a GnuTLS-linked host program picks up the bundle's
// stale libnghttp2 and dies with an undefined-symbol error (#334). Returns true
// when a sanitized environment was produced (pass &env to wxExecute); false
// when not inside an AppImage, in which case the child should inherit the
// environment unchanged.
//
// Lives in COMMON so both the daemon and the GUI can share it; it has no GUI
// dependencies and is a no-op (returns false) on every platform that does not
// set $APPDIR.
bool GetSanitizedExecEnv(wxExecuteEnv &env);

} // namespace AppImageEnv

#endif // APPIMAGEENV_H
