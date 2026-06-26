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
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
//

#ifndef LIBWEBCOMMON_ROLE_H
#define LIBWEBCOMMON_ROLE_H

// Role enumeration shared between the REST API dispatcher and the JWT
// machinery. Kept in its own tiny header so the rest of libwebcommon
// (and the API handlers that consume it) can refer to roles without
// pulling in the full Jwt header.
enum class Role
{
	PUBLIC, // No credentials required (login, version)
	GUEST,  // Authenticated as any role (read-only endpoints)
	ADMIN   // Authenticated as admin (mutating endpoints)
};

#endif // LIBWEBCOMMON_ROLE_H
