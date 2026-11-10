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
