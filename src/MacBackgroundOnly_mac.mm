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

#include <ApplicationServices/ApplicationServices.h>

#include "MacBackgroundOnly_mac.h"

void MacDeclareBackgroundOnly()
{
#if MAC_OS_X_VERSION_MAX_ALLOWED >= 1070
	ProcessSerialNumber psn = { 0, kCurrentProcess };
	TransformProcessType(&psn, kProcessTransformToBackgroundApplication);
#endif
}

#ifndef AMULE_DAEMON
// Run before main(), so wxEntry() cannot register this helper process as a foreground app.
__attribute__((constructor)) static void DeclareBackgroundOnlyBeforeMain()
{
	MacDeclareBackgroundOnly();
}
#endif
