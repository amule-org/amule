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

#ifndef EC_CLIENT_PASSWORD_H
#define EC_CLIENT_PASSWORD_H

#include <wx/config.h>

namespace ECClientPassword
{
// The server has already committed next. Always update the in-memory reconnect
// credential, but only replace a saved credential for this exact connection.
// In particular, rotating a password must not opt a user into saving passwords.
inline bool Commit(
	wxConfigBase &cfg, const wxString &host, int port, wxString &sessionPassword, const wxString &next)
{
	const bool remembered = cfg.Read("/EC/Host", wxString()) == host &&
				cfg.Read("/EC/Port", 4712L) == port && !sessionPassword.IsEmpty() &&
				cfg.Read("/EC/Password", wxString()) == sessionPassword;
	sessionPassword = next;
	return !remembered || (cfg.Write("/EC/Password", next) && cfg.Flush());
}
} // namespace ECClientPassword

#endif
