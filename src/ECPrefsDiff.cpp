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

#include "ECPrefsDiff.h"

namespace
{
bool IsPresenceFlag(const CECTag &tag)
{
	return tag.IsCustom() && tag.GetTagDataLen() == 0;
}

CECTag ToFullDetail(const CECTag &tag)
{
	if (!IsPresenceFlag(tag)) {
		return tag;
	}
	// The guest-password containers are flags too: the value is the toggle, the child the hash.
	CECTag flag(tag.GetTagName(), static_cast<uint8_t>(1));
	for (const CECTag &child : tag) {
		flag.AddTag(CECTag(child));
	}
	return flag;
}

void DiffCategory(const CECTag &base, const CECTag &current, CECTag &out)
{
	for (const CECTag &tag : current) {
		const CECTag *old = base.GetTagByName(tag.GetTagName());
		if (old == nullptr || *old != tag) {
			out.AddTag(ToFullDetail(tag));
		}
	}
	for (const CECTag &tag : base) {
		// Only a flag can be switched off. A password hash that is not sent means "unchanged",
		// and the amuleapi admin password has no off state by design.
		if (IsPresenceFlag(tag) && tag.GetTagName() != EC_TAG_AMULEAPI_PASSWD &&
			current.GetTagByName(tag.GetTagName()) == nullptr) {
			out.AddTag(CECTag(tag.GetTagName(), static_cast<uint8_t>(0)));
		}
	}
}
} // namespace

std::unique_ptr<CECPacket> MakePrefsDiffPacket(const CECPacket &base, const CECPacket &current)
{
	auto diff = std::make_unique<CECPacket>(EC_OP_SET_PREFERENCES, EC_DETAIL_FULL);
	for (const CECTag &category : current) {
		const CECEmptyTag none(category.GetTagName());
		const CECTag *old = base.GetTagByName(category.GetTagName());
		CECEmptyTag changes(category.GetTagName());
		DiffCategory(old != nullptr ? *old : none, category, changes);
		if (changes.GetTagCount() > 0) {
			diff->AddTag(changes);
		}
	}
	return diff;
}
