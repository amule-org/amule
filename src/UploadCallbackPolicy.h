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

#ifndef UPLOAD_CALLBACK_POLICY_H
#define UPLOAD_CALLBACK_POLICY_H

#include "SourceRoutePolicy.h" // Needed for EContactRouteReport

#include <cstdint>

// The upload queue has no unit-test seam, so its rules live here where they can be tested.
// This header keeps the policy pure and independent of application and network state.

// A callback stops counting against the in-flight limit after this long, or at the hello, whichever comes
// first.
constexpr std::uint64_t kUploadCallbackInFlightMs = 30'000;
// Uploads may occupy at most this share of the shared cross-server target budget; the budget
// itself is kForeignTargetsPerWindow, so the download side keeps the whole of it.
constexpr unsigned kUploadCallbackForeignTargetShare = 15;

//! A callback may start unless this peer was already spent in the current request interval.
//!
//! Only the spent marker decides: the sent time is cleared on every disconnect and on the answer,
//! so comparing against it would re-arm the callback after any socket close and the marker would
//! never bite. SetLastUpRequest() clears the marker when the peer's own request arrives.
inline bool MayStartUploadCallback(bool spent)
{
	return !spent;
}

inline bool IsUploadCallbackInFlight(std::uint64_t sentAtMs, std::uint64_t nowMs)
{
	return sentAtMs != 0 && nowMs >= sentAtMs && nowMs - sentAtMs < kUploadCallbackInFlightMs;
}

inline unsigned MaxInflightUploadCallbacks(unsigned freeSlots, bool lowIdAlternationSlot)
{
	return freeSlots + (lowIdAlternationSlot ? 1U : 0U);
}

//! The peer drops, unanswered, a direct callback request that comes within @a throttleMs of the
//! last one from our IP.
inline bool IsDirectCallbackThrottled(std::uint64_t lastSentMs, std::uint64_t nowMs, std::uint64_t throttleMs)
{
	return lastSentMs != 0 && nowMs >= lastSentMs && nowMs - lastSentMs < throttleMs;
}

struct SUploadCallbackRouteFacts
{
	//! CanDoCallback() for the peer's server: the peer can connect to us.
	bool canTakeCallback = false;
	//! CUpDownClient::IsDirectCallbackPossible().
	bool directCallback = false;
	bool directThrottled = false;
	bool onOurServer = false;
};

/**
 * The upload callback route that needs no NAT-T server support, or None.
 *
 * The order is TryToContact()'s. The server callback needs the peer on our server: a LowID is valid
 * only on its own server, so the request would wake a different client on ours.
 */
inline EContactRouteReport UploadCallbackRoute(const SUploadCallbackRouteFacts &facts)
{
	if (!facts.canTakeCallback) {
		return EContactRouteReport::None;
	}
	if (facts.directCallback && !facts.directThrottled) {
		return EContactRouteReport::DirectUdpCallback;
	}
	return facts.onOurServer ? EContactRouteReport::ServerCallback : EContactRouteReport::None;
}

#endif // UPLOAD_CALLBACK_POLICY_H
