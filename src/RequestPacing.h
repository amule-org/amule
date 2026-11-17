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

// The source's CheckForAggressive() score after one more request: 3 more if it comes less than
// MIN_REQUESTTIME after the previous one, else 1 less. eMule's AddRequestCount() adds and takes
// 1, so its count never exceeds this score.
inline unsigned AggressiveScoreAfter(unsigned score, uint64_t now, uint64_t lastPacedRequest)
{
	if (lastPacedRequest != 0 && now - lastPacedRequest < MIN_REQUESTTIME) {
		return score + 3;
	}
	return score > 0 ? score - 1 : 0;
}

// An early ask adds 3. Asking early only at 0 keeps our early asks from lifting aMule's score
// above 3 (ban at 10) or eMule's count above 1 (ban at 4).
inline bool MayAskEarly(unsigned score)
{
	return score == 0;
}

// An OP_AICHREQUEST comes between two asks, so the next ask can be early. Send one only where
// MayAskEarly would still allow that ask.
inline bool MayRequestAICH(unsigned score, uint64_t now, uint64_t lastPacedRequest)
{
	return MayAskEarly(AggressiveScoreAfter(score, now, lastPacedRequest));
}

#endif // REQUESTPACING_H
// File_checked_for_headers
