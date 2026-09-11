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

// These functions cannot live in ECTag.cpp, because they use non-inline member functions of
// CUInt128. This way they are only needed if the CUInt128 class is actually used, and cause no
// linker errors for apps like aMuleCmd that do not use it. The other option would be to make them
// all inline members of CECTag, which would clutter the header too much.

#include "ECTag.h"
#include "ECSpecialTags.h"
#include "../../../kademlia/utils/UInt128.h"

CECTag::CECTag(ec_tagname_t tagname, const CUInt128 &data)
: m_tagName(tagname)
, m_dataType(EC_TAGTYPE_UINT128)
, m_dataLen(16)
{
	NewData();
	data.ToByteArray(reinterpret_cast<uint8_t *>(m_tagData));
}

void CECTag::AddTag(ec_tagname_t name, const CUInt128 &data, CValueMap *valuemap)
{
	if (valuemap) {
		valuemap->CreateTag(name, data, this);
	} else {
		AddTag(CECTag(name, data));
	}
}

CUInt128 CECTag::GetInt128Data() const
{
	if (m_dataType != EC_TAGTYPE_UINT128) {
		EC_ASSERT(m_dataType == EC_TAGTYPE_UNKNOWN);
		return CUInt128(false);
	}

	EC_ASSERT(m_tagData != NULL);

	if (m_tagData) {
		return CUInt128(reinterpret_cast<const uint8_t *>(m_tagData));
	} else {
		return CUInt128(false);
	}
}

CUInt128 CECTag::AssignIfExist(ec_tagname_t tagname, CUInt128 *target) const
{
	if (AssignIfExist(tagname, *target)) {
		return *target;
	} else {
		return CUInt128(false);
	}
}

bool CECTag::AssignIfExist(ec_tagname_t tagname, CUInt128 &target) const
{
	const CECTag *tag = GetTagByName(tagname);
	if (tag) {
		target = tag->GetInt128Data();
		return true;
	}
	return false;
}
