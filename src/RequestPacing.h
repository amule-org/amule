//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
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
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
//

#ifndef REQUESTPACING_H
#define REQUESTPACING_H

#include "Types.h"                   // Needed for uint64, needed by Constants.h
#include <protocol/ed2k/Constants.h> // Needed for MIN_REQUESTTIME

#include <cstdint>

// A source's CheckForAggressive() scores an OP_STARTUPLOADREQ, OP_REASKFILEPING or OP_AICHREQUEST
// sent less than MIN_REQUESTTIME after the previous one, and bans at 10. Returns the last-asked
// time that makes CPartFile::Process(), which asks reaskTime after it, ask at the first moment
// that is not scored: now, or MIN_REQUESTTIME after lastPacedRequest (0 if there was none).
inline uint64_t PacedLastAskedTime(uint64_t now, uint64_t lastPacedRequest, uint64_t reaskTime)
{
	const uint64_t askAt = lastPacedRequest ? lastPacedRequest + MIN_REQUESTTIME : 0;
	if (askAt <= now) {
		return 0; // Process() asks on its next tick
	}
	return askAt > reaskTime ? askAt - reaskTime : 1;
}

#endif // REQUESTPACING_H
// File_checked_for_headers
