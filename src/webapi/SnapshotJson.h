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

#ifndef WEBAPI_SNAPSHOTJSON_H
#define WEBAPI_SNAPSHOTJSON_H

#include "State.h"

class CJsonWriter;

namespace webapi
{

// Includes the "network" key and its object, shared by /kad and /status.
void WriteKadNetworkObject(CJsonWriter &w, const KadSnapshot &k);

// The complete /status and status_changed object from one coherent dashboard read.
// The refresh timestamp is deliberately excluded so it cannot invalidate HTTP caches.
void WriteStatusObject(CJsonWriter &w, const CState::DashboardSnapshot &dashboard);

} // namespace webapi

#endif // WEBAPI_SNAPSHOTJSON_H
