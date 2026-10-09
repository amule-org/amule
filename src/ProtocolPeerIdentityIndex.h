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

#ifndef PROTOCOLPEERIDENTITYINDEX_H
#define PROTOCOLPEERIDENTITYINDEX_H

#include "ProtocolPeerIdentity.h"
#include <map>
#include <optional>

template <typename Value> class CProtocolPeerIdentityIndex
{
public:
	void Add(const CProtocolPeerIdentity &key, const Value &value)
	{
		if (key.GetKind() == CProtocolPeerIdentity::Kind::NativeIPv6)
			m_entries.emplace(key, value);
	}

	std::optional<Value> Find(const CProtocolPeerIdentity &key) const
	{
		if (key.GetKind() != CProtocolPeerIdentity::Kind::NativeIPv6)
			return std::nullopt;
		auto it = m_entries.find(key);
		return it == m_entries.end() ? std::nullopt : std::optional<Value>(it->second);
	}

	void Remove(const CProtocolPeerIdentity &key, const Value &value)
	{
		if (key.GetKind() != CProtocolPeerIdentity::Kind::NativeIPv6)
			return;
		auto range = m_entries.equal_range(key);
		for (auto it = range.first; it != range.second; ++it) {
			if (it->second == value) {
				m_entries.erase(it);
				return;
			}
		}
	}

	void Update(
		const CProtocolPeerIdentity &oldKey, const CProtocolPeerIdentity &newKey, const Value &value)
	{
		Remove(oldKey, value);
		Add(newKey, value);
	}

	void Clear() { m_entries.clear(); }

private:
	std::multimap<CProtocolPeerIdentity, Value> m_entries;
};

#endif
