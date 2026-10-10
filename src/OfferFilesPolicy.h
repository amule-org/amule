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

#include "MD4Hash.h"
#include <set>
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
	uint32_t BatchLimit(uint32_t alreadyOffered, uint32_t liveSoft = 0, uint32_t liveHard = 0) const;
	uint32_t IntervalMs() const;
	uint32_t PublicationIntervalMs(bool accelerated, uint32_t legacyIntervalMs) const;

private:
	std::array<uint32_t, 5> m_values{};
	uint8_t m_seen = 0;
	bool m_invalid = false;
};

// Counts distinct candidates, including legacy offers before negotiation. Socket-owned.
class COfferFilesPublication
{
public:
	void Record(const CMD4Hash &hash) { m_offered.insert(hash); }
	bool Contains(const CMD4Hash &hash) const { return m_offered.count(hash) != 0; }
	uint32_t Count() const { return static_cast<uint32_t>(m_offered.size()); }
	bool Due(uint64_t now, uint32_t interval) const { return !m_sent || now - m_lastSent >= interval; }
	void Sent(uint64_t now)
	{
		m_sent = true;
		m_lastSent = now;
	}
	void Reset() { *this = COfferFilesPublication(); }

private:
	std::set<CMD4Hash> m_offered;
	uint64_t m_lastSent = 0;
	bool m_sent = false;
};

// Socket-owned snapshot. Begin before decoding so truncation cannot preserve
// an earlier policy. Duplicate SERVERIDENT packets invalidate the snapshot.
// A valid snapshot remains binding until disconnect, even if the experimental
// preference is switched off after the login requested v1.
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
