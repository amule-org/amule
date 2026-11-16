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

#include <array>
#include <cstddef>
#include <cstdint>

//! Kad request handlers whose receiver-key observations are tracked.
enum class EKadReceiverKeyHandler : std::size_t
{
	KeywordPublish,
	SourcePublish,
	NotesPublish,
	FirewallCheck,
	Firewall2Check,
	FindBuddy,
	Callback,
	Count
};

/**
 * Whether a whole request is dropped because its receiver key did not prove the sender.
 *
 * Only the Kad2 firewall check. It is sent from a hello exchange, where the peer already holds our
 * key, and the clients that send it always obfuscate, so a missing key means the packet did not
 * come from the address it names. The handler dials that address, so a spoofed one makes us dial a
 * stranger on a port the packet chose. The other handlers still see honest requests without a key,
 * so gating one of them wholesale would cost more than it protects (#2315). A source publish is
 * decided per record instead, by DropsUnkeyedSourcePublish().
 */
inline bool DropsWithoutReceiverKey(EKadReceiverKeyHandler handler, bool validReceiverKey) noexcept
{
	return !validReceiverKey && handler == EKadReceiverKeyHandler::Firewall2Check;
}

/**
 * Whether a source publish is dropped because its receiver key did not prove the sender.
 *
 * Only source type 7, which carries the rule from the day it was defined, so no publisher of it
 * predates the key. Classic types cannot be gated: most of the publishes a live node sees arrive
 * without a key (#2315). The key proves the publisher receives at the IPv4 we stamp into
 * TAG_SOURCEIP, the field a receiver compares a record against.
 */
inline bool DropsUnkeyedSourcePublish(uint8_t sourceType, bool validReceiverKey) noexcept
{
	return !validReceiverKey && sourceType == 7;
}

//! Valid and missing receiver keys observed by each handler.
class CKadReceiverKeyCounts
{
public:
	void Record(EKadReceiverKeyHandler handler, bool validReceiverKey)
	{
		++(validReceiverKey ? m_valid : m_missing)[static_cast<std::size_t>(handler)];
	}

	uint64_t GetValid(EKadReceiverKeyHandler handler) const
	{
		return m_valid[static_cast<std::size_t>(handler)];
	}

	uint64_t GetMissing(EKadReceiverKeyHandler handler) const
	{
		return m_missing[static_cast<std::size_t>(handler)];
	}

private:
	std::array<uint64_t, static_cast<std::size_t>(EKadReceiverKeyHandler::Count)> m_valid{};
	std::array<uint64_t, static_cast<std::size_t>(EKadReceiverKeyHandler::Count)> m_missing{};
};

#endif // KADRECEIVERKEYPOLICY_H
