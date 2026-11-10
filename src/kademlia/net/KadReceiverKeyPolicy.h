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

#ifndef KADRECEIVERKEYPOLICY_H
#define KADRECEIVERKEYPOLICY_H

#include <cstdint>

/**
 * What to do with a Kad request whose receiver key does not prove the sender's address.
 *
 * A valid key proves the sender receives at the IP it claims, so a request carrying one is always
 * processed. Without one there are two honest outcomes and the choice is the user's: count it while
 * enforcement is off, because eMule official sends no key and dropping those requests would cost
 * legitimate sources, or reject it once the user has decided the sources are worth the risk.
 */
enum class EKadReceiverKeyDecision
{
	Process,         //!< The key is valid; nothing to count and nothing to reject.
	ProcessAndCount, //!< No key and enforcement is off: process, and record that we would have rejected.
	Reject,          //!< No key and enforcement is on: do not act on the packet's source address.
};

//! The decision for one request, given only the key result and the preference.
inline EKadReceiverKeyDecision DecideKadReceiverKeyRequest(bool validReceiverKey, bool enforce)
{
	if (validReceiverKey) {
		return EKadReceiverKeyDecision::Process;
	}
	return enforce ? EKadReceiverKeyDecision::Reject : EKadReceiverKeyDecision::ProcessAndCount;
}

#endif // KADRECEIVERKEYPOLICY_H
