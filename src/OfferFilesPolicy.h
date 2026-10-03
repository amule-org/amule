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

#ifndef OFFERFILESPOLICY_H
#define OFFERFILESPOLICY_H

#include <array>
#include <cstdint>
class CTag;

// Draft v1 advertisement, validated as one packet; never stored in server.met.
class COfferFilesAdvertisement
{
public:
	void AddTag(const CTag &tag);
	bool IsValid() const;
	uint32_t AdvertisedBatchLimit() const { return m_values[1]; }
	uint32_t AdvertisedIntervalMs() const { return m_values[2]; }
	uint32_t SoftLimit() const { return m_values[3]; }
	uint32_t HardLimit() const { return m_values[4]; }
	uint32_t BatchLimit(uint32_t alreadyOffered) const;
	uint32_t IntervalMs() const;

private:
	std::array<uint32_t, 5> m_values{};
	uint8_t m_seen = 0;
	bool m_invalid = false;
};

// Socket-owned snapshot. Begin before decoding so truncation cannot preserve
// an earlier policy. Duplicate SERVERIDENT packets invalidate the snapshot.
// A valid snapshot is diagnostic only until the accelerated publisher is wired.
class COfferFilesConnectionPolicy
{
public:
	bool BeginAdvertisement();
	void RejectAdvertisement();
	void Commit(const COfferFilesAdvertisement &advertisement);
	void Reset() { *this = COfferFilesConnectionPolicy(); }
	const COfferFilesAdvertisement *Get() const { return m_valid ? &m_snapshot : nullptr; }

private:
	COfferFilesAdvertisement m_snapshot;
	bool m_seen = false;
	bool m_pending = false;
	bool m_valid = false;
};
#endif
