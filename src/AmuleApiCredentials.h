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

#ifndef AMULEAPICREDENTIALS_H
#define AMULEAPICREDENTIALS_H

class wxString;

/**
 * aMule's side of amuleapi's admin/guest credentials.
 *
 * The credentials live in one file, amuleapi-passwords, in the aMule config
 * dir. amuleapi reads it; amuleapi, amuled and monolithic aMule all write
 * it. The format and the hashing belong to webcommon/Credentials.h so the
 * three cannot drift apart — this header is only the glue that connects
 * that store to aMule's preferences.
 *
 * Not compiled into amulegui: a remote GUI has no local credential file,
 * and its config dir belongs to a different machine. amulegui asks the
 * daemon to make the change over EC instead, and the daemon lands here.
 */
namespace AmuleApiCredentials
{

/**
 * Writes whatever the preferences are currently asking for into
 * amuleapi-passwords, then refreshes the state mirrors.
 *
 * The two password preferences are pending requests, not stored values:
 *
 *  - a non-empty admin digest sets the admin password;
 *  - guest disabled clears the guest credential, which is what turns guest
 *    access off;
 *  - guest enabled with a non-empty digest sets the guest password;
 *  - guest enabled with an empty digest leaves the stored guest password
 *    alone, so an EC client can change the port without having to resend a
 *    password it can never read back.
 *
 * Each applied digest is cleared from the preferences afterwards, so the
 * request is not replayed on the next save.
 *
 * Returns false and fills `error` if the file could not be written.
 * Preferences that were applied before the failure stay applied — each
 * role is written separately, and a half-applied change is better left
 * visible than silently rolled back to a password the user thinks they
 * replaced.
 */
bool ApplyPrefs(wxString &error);

/**
 * Reads amuleapi-passwords and updates the "admin is set" / "guest is
 * enabled" preference mirrors from it. Called at startup and after every
 * change, so the preferences dialog shows what is actually stored rather
 * than what was last typed.
 */
void RefreshState();

} // namespace AmuleApiCredentials

#endif // AMULEAPICREDENTIALS_H
