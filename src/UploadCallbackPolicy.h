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

struct SUploadCallbackRouteFacts
{
	//! CanDoCallback() for the peer's server: the peer can connect to us.
	bool canTakeCallback = false;
	//! CUpDownClient::IsDirectCallbackPossible().
	bool directCallback = false;
	bool directThrottled = false;
	//! A direct request went out in this request interval and was not answered.
	bool directTried = false;
	bool onOurServer = false;
	//! CUpDownClient::IsUploadKadBuddyCallbackPossible().
	bool kadBuddyCallback = false;
	//! A Kad callback request went to the peer's buddy within eMule's flood interval.
	bool kadBuddyThrottled = false;
};

/**
 * The upload callback route that needs no NAT-T server support, or None.
 *
 * The order is TryToContact()'s: an unanswered direct request moves on to the next route. The server
 * callback needs the peer on our server: a LowID is valid only on its own server, so the request
 * would wake a different client on ours. The Kad buddy route needs a file we download from the
 * peer: only that file's sources give its buddy ID.
 */
inline EContactRouteReport UploadCallbackRoute(const SUploadCallbackRouteFacts &facts)
{
	if (!facts.canTakeCallback) {
		return EContactRouteReport::None;
	}
	if (facts.directCallback && !facts.directThrottled && !facts.directTried) {
		return EContactRouteReport::DirectUdpCallback;
	}
	if (facts.onOurServer) {
		return EContactRouteReport::ServerCallback;
	}
	if (facts.kadBuddyCallback && !facts.kadBuddyThrottled) {
		return EContactRouteReport::KadBuddy;
	}
	return EContactRouteReport::None;
}

//! Whether a request on @a route spends the peer when it is sent. A direct request spends it only
//! when answered: the peer ignores one it will not take, and the next route is still worth a try.
inline bool UploadCallbackSpendsPeer(EContactRouteReport route)
{
	return route != EContactRouteReport::DirectUdpCallback;
}

#endif // UPLOAD_CALLBACK_POLICY_H
