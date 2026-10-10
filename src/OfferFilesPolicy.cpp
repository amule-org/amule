//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2002-2011 Merkur ( devs@emule-project.net / http://www.emule-project.net )
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

#include "OfferFilesPolicy.h"
#include "Tag.h"
#include <tags/ServerTags.h>
#include <algorithm>
#include <limits>

void COfferFilesAdvertisement::AddTag(const CTag &tag)
{
	int field = -1;
	if (tag.GetNameID() == ST_SOFTFILES) {
		field = 3;
	} else if (tag.GetNameID() == ST_HARDFILES) {
		field = 4;
	} else if (tag.GetNameID() == 0) {
		if (tag.GetName() == "offerfiles_v") {
			field = 0;
		} else if (tag.GetName() == "offerfiles_batch_max") {
			field = 1;
		} else if (tag.GetName() == "offerfiles_min_interval_ms") {
			field = 2;
		}
	}
	if (field < 0) {
		return;
	} // Unknown extensions are safely ignorable.
	const uint8_t bit = uint8_t(1u << field);
	if ((m_seen & bit) || tag.GetType() != TAGTYPE_UINT32) {
		m_invalid = true;
	}
	m_seen |= bit;
	if (tag.GetType() == TAGTYPE_UINT32) {
		const uint64 value = tag.GetInt();
		if (value > std::numeric_limits<uint32_t>::max()) {
			m_invalid = true;
		} else {
			m_values[field] = static_cast<uint32_t>(value);
		}
	}
}

bool COfferFilesAdvertisement::IsValid() const
{
	return !m_invalid && m_seen == 31 && m_values[0] == 1 && m_values[1] > 0 && m_values[2] > 0 &&
	       SoftLimit() > 0 && HardLimit() > m_values[1];
}

uint32_t COfferFilesAdvertisement::BatchLimit(
	uint32_t alreadyOffered, uint32_t liveSoft, uint32_t liveHard) const
{
	if (!IsValid() || alreadyOffered >= SoftLimit()) {
		return 0;
	}
	uint32_t limit =
		std::min({ SoftLimit() - alreadyOffered, m_values[1], HardLimit() - 1, uint32_t(200) });
	// Live limits can restrict but never expand the negotiated snapshot.
	if (liveSoft) {
		limit = std::min(limit, alreadyOffered >= liveSoft ? 0u : liveSoft - alreadyOffered);
	}
	if (liveHard) {
		limit = std::min(limit, liveHard - 1);
	}
	return limit;
}

uint32_t COfferFilesAdvertisement::IntervalMs() const
{
	// At most 200 records / 500 ms = 400 records/s. No burst entitlement.
	return IsValid() ? std::max(m_values[2], uint32_t(500)) : 60000;
}

uint32_t COfferFilesAdvertisement::PublicationIntervalMs(bool accelerated, uint32_t legacyIntervalMs) const
{
	return accelerated ? IntervalMs() : std::max(IntervalMs(), legacyIntervalMs);
}

bool COfferFilesConnectionPolicy::BeginAdvertisement()
{
	const bool first = !m_seen;
	RejectAdvertisement();
	m_pending = first;
	return first;
}

void COfferFilesConnectionPolicy::RejectAdvertisement()
{
	m_seen = true;
	m_pending = false;
	m_valid = false;
	m_snapshot = COfferFilesAdvertisement();
}

void COfferFilesConnectionPolicy::Commit(const COfferFilesAdvertisement &advertisement)
{
	if (m_pending) {
		m_snapshot = advertisement;
		m_valid = advertisement.IsValid();
		m_pending = false;
	}
}
