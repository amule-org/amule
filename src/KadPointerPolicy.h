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

#ifndef KAD_POINTER_POLICY_H
#define KAD_POINTER_POLICY_H

#include <cstdint>

#include "KadPublishPolicy.h" // Needed for EKadSourcePublish

// The buddy pointer (#2513, emule-qt ipv6-spec §5.6.1): a LowID publishes its current buddy once,
// at a key every receiver can compute from its source ID, so records that name an older buddy
// still lead to it.

//! The `bpv` value this client publishes.
constexpr uint8_t KAD_POINTER_VERSION = 1;
//! How soon a pointer no storer acknowledged is published again.
constexpr uint32_t KAD_POINTER_RETRY_SECONDS = 600;

/**
 * The key of our pointer record: the bitwise NOT of the 16 source-ID bytes. The NOT commutes with
 * any byte order, so every client computes the same key whatever layout it keeps the ID in.
 */
inline void KadPointerKey(const uint8_t *sourceID, uint8_t *key) noexcept
{
	for (unsigned i = 0; i < 16; ++i) {
		key[i] = static_cast<uint8_t>(~sourceID[i]);
	}
}

/**
 * Whether a record to this storer carries the pointer route, and whether the pointer record goes to
 * it. A storer following §5.6 drops both without our receiver key, so an unkeyed storer gets the
 * plain record and no pointer.
 */
inline bool SendsKadPointer(bool enabled, bool storerKeyHeld) noexcept
{
	return enabled && storerKeyHeld;
}

//! The buddy our last pointer named, and when it is due again.
struct SKadPointerState
{
	uint32_t buddyIP = 0;
	uint16_t buddyPort = 0;
	uint32_t nextDue = 0;
};

/**
 * Whether to publish the pointer now: only while we publish through a buddy, at once when that
 * buddy is not the one the pointer names, and otherwise when the pointer is due again. Without a
 * buddy nothing goes out, and the last pointer expires on its storers.
 */
inline bool IsKadPointerDue(bool enabled,
	EKadSourcePublish mode,
	uint32_t buddyIP,
	uint16_t buddyPort,
	const SKadPointerState &state,
	uint32_t now) noexcept
{
	return enabled && mode == EKadSourcePublish::Buddy && buddyIP != 0 &&
	       (buddyIP != state.buddyIP || buddyPort != state.buddyPort || now >= state.nextDue);
}

//! The state after a pointer lookup for this buddy started; storers keep it `lifetime` seconds.
inline SKadPointerState KadPointerStarted(
	uint32_t buddyIP, uint16_t buddyPort, uint32_t now, uint32_t lifetime) noexcept
{
	return { buddyIP, buddyPort, now + lifetime };
}

//! A pointer lookup ended: with no acknowledgement, it is tried again sooner.
inline void KadPointerFinished(SKadPointerState &state, uint32_t acknowledged, uint32_t now) noexcept
{
	if (acknowledged == 0 && now + KAD_POINTER_RETRY_SECONDS < state.nextDue) {
		state.nextDue = now + KAD_POINTER_RETRY_SECONDS;
	}
}

#endif // KAD_POINTER_POLICY_H
