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

#ifndef EXTSOURCEEXCHANGE_H
#define EXTSOURCEEXCHANGE_H

#include "KadSourceAddressPolicy.h" // Needed for ShouldContactKadSourceOverIPv6
#include "MD4Hash.h"
#include "NetworkFunctions.h" // Needed for IsLowID
#include "SafeFile.h"
#include "Tag.h"
#include <tags/ClientTags.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

/**
 * Extended Source Exchange (eMuleAI, emule-qt): an SX2 answer with version byte
 * SOURCEEXCHANGEEXT_VERSION whose records are `<id 4><port 2><tag count 1><tags>`, with no
 * fixed tail. The tag set for LowID routes was agreed on amule-org/amule#2456.
 */
namespace ExtSX
{
using Bytes16 = std::array<uint8_t, 16>;

//! One source. IPv4 fields are in network order, as everywhere in the ed2k protocol.
struct SRecord
{
	uint32_t hybridID = 0;
	uint16_t port = 0;
	uint32_t serverIP = 0;
	uint16_t serverPort = 0;
	std::optional<Bytes16> ipv6;
	std::optional<CMD4Hash> userHash;
	std::optional<uint8_t> connectOptions;
	// LowID route.
	uint32_t callbackIP = 0;
	uint16_t udpPort = 0;
	uint16_t kadPort = 0;
	uint32_t buddyIP = 0;
	std::optional<Bytes16> buddyIPv6;
	uint16_t buddyPort = 0;
	//! CUInt128::ToByteArray order, as CUpDownClient stores it and 0xBF carries it.
	std::optional<Bytes16> buddyID;
};

//! The tags eMuleAI reads. On any other tag it drops the rest of the answer (eMuleAI/eMuleAI#171),
//! so a peer without MOD_MISCOPT_EXTXS_SKIP_TAGS gets only these.
inline bool IsTagSafeForEveryReader(uint8_t id)
{
	switch (id) {
	case CT_EMULE_ADDRESS:
	case CT_EMULE_UDPPORTS:
	case CT_EMULE_BUDDYIP:
	case CT_EMULE_BUDDYUDP:
	case CT_EMULE_SERVINGBUDDYIPV6:
	case CT_EMULE_SERVINGBUDDYID:
	case CT_EMULE_SERVERIP:
	case CT_EMULE_SERVERTCP:
	case CT_MOD_IP_V6:
		return true;
	default:
		return false;
	}
}

inline Bytes16 ToBytes16(const CMD4Hash &hash)
{
	Bytes16 bytes;
	std::copy_n(hash.GetHash(), bytes.size(), bytes.begin());
	return bytes;
}

//! Unknown tags, and known ones of the wrong type, are skipped. Throws as CTag does on a
//! malformed or truncated record.
inline SRecord ReadRecord(const CFileDataIO &data)
{
	SRecord record;
	// htonl(hybrid ID) for HighIDs and LowIDs alike, as eMuleAI and emule-qt write it. Read
	// first: the swap macro evaluates its argument more than once.
	const uint32_t wireID = data.ReadUInt32();
	record.hybridID = wxUINT32_SWAP_ALWAYS(wireID);
	record.port = data.ReadUInt16();
	const uint8_t tagCount = data.ReadUInt8();
	for (uint8_t i = 0; i < tagCount; ++i) {
		const CTag tag(data, true);
		const uint32_t value = tag.IsInt() ? static_cast<uint32_t>(tag.GetInt()) : 0;
		switch (tag.GetNameID()) {
		case CT_EMULE_SERVERIP:
			if (tag.IsInt()) {
				record.serverIP = value;
			}
			break;
		case CT_EMULE_SERVERTCP:
			if (tag.IsInt()) {
				record.serverPort = static_cast<uint16_t>(value);
			}
			break;
		case CT_MOD_IP_V6:
			if (tag.IsHash()) {
				record.ipv6 = ToBytes16(tag.GetHash());
			}
			break;
		case CT_EMULE_USERHASH:
			if (tag.IsHash()) {
				record.userHash = tag.GetHash();
			}
			break;
		case CT_EMULE_CONOPTS:
			if (tag.IsInt()) {
				record.connectOptions = static_cast<uint8_t>(value);
			}
			break;
		case CT_EMULE_ADDRESS:
			if (tag.IsInt()) {
				record.callbackIP = value;
			}
			break;
		case CT_EMULE_UDPPORTS:
			if (tag.IsInt()) {
				record.kadPort = static_cast<uint16_t>(value >> 16);
				record.udpPort = static_cast<uint16_t>(value);
			}
			break;
		case CT_EMULE_BUDDYIP:
			if (tag.IsInt()) {
				record.buddyIP = value;
			}
			break;
		case CT_EMULE_SERVINGBUDDYIPV6:
			if (tag.IsHash()) {
				record.buddyIPv6 = ToBytes16(tag.GetHash());
			}
			break;
		case CT_EMULE_BUDDYUDP:
			if (tag.IsInt()) {
				record.buddyPort = static_cast<uint16_t>(value);
			}
			break;
		case CT_EMULE_SERVINGBUDDYID:
			if (tag.IsHash()) {
				record.buddyID = ToBytes16(tag.GetHash());
			}
			break;
		default:
			break;
		}
	}
	return record;
}

inline void WriteRecord(CFileDataIO &data, const SRecord &record, bool peerSkipsUnknownTags)
{
	std::vector<std::unique_ptr<CTag>> tags;
	const auto addInt = [&tags](uint8_t id, uint32_t value) {
		tags.push_back(std::make_unique<CTagVarInt>(id, value));
	};
	const auto addHash = [&tags](uint8_t id, const uint8_t *bytes) {
		tags.push_back(std::make_unique<CTagHash>(id, CMD4Hash(bytes)));
	};

	if (IsLowID(record.hybridID)) {
		if (record.callbackIP != 0) {
			addInt(CT_EMULE_ADDRESS, record.callbackIP);
			addInt(CT_EMULE_UDPPORTS, (uint32_t{ record.kadPort } << 16) | record.udpPort);
		}
		if (record.buddyPort != 0 && (record.buddyIP != 0 || record.buddyIPv6)) {
			if (record.buddyIP != 0) {
				addInt(CT_EMULE_BUDDYIP, record.buddyIP);
			}
			if (record.buddyIPv6) {
				addHash(CT_EMULE_SERVINGBUDDYIPV6, record.buddyIPv6->data());
			}
			addInt(CT_EMULE_BUDDYUDP, record.buddyPort);
		}
		if (record.buddyID) {
			addHash(CT_EMULE_SERVINGBUDDYID, record.buddyID->data());
		}
	}
	if (record.serverIP != 0) {
		addInt(CT_EMULE_SERVERIP, record.serverIP);
		addInt(CT_EMULE_SERVERTCP, record.serverPort);
	}
	if (record.ipv6) {
		addHash(CT_MOD_IP_V6, record.ipv6->data());
	}
	if (record.userHash) {
		addHash(CT_EMULE_USERHASH, record.userHash->GetHash());
	}
	if (record.connectOptions) {
		addInt(CT_EMULE_CONOPTS, *record.connectOptions);
	}

	if (!peerSkipsUnknownTags) {
		tags.erase(std::remove_if(tags.begin(),
				   tags.end(),
				   [](const std::unique_ptr<CTag> &tag) {
					   return !IsTagSafeForEveryReader(tag->GetNameID());
				   }),
			tags.end());
	}

	data.WriteUInt32(wxUINT32_SWAP_ALWAYS(record.hybridID));
	data.WriteUInt16(record.port);
	data.WriteUInt8(static_cast<uint8_t>(tags.size()));
	for (const auto &tag : tags) {
		tag->WriteNewEd2kTag(&data);
	}
}

//! IPv4 in network order.
struct SAddressFilter
{
	std::function<bool(uint32_t)> acceptIPv4;
	std::function<bool(const Bytes16 &)> acceptIPv6;
};

//! Drops the addresses aMule must not use or pass on.
inline void SanitizeRoute(SRecord &record, const SAddressFilter &filter)
{
	if (record.ipv6 && !filter.acceptIPv6(*record.ipv6)) {
		record.ipv6.reset();
	}
	if (!IsLowID(record.hybridID)) {
		// A HighID is dialled on its ID.
		record.callbackIP = 0;
		record.udpPort = 0;
		record.kadPort = 0;
		record.buddyIP = 0;
		record.buddyIPv6.reset();
		record.buddyPort = 0;
		record.buddyID.reset();
		return;
	}
	if (record.callbackIP != 0 && !filter.acceptIPv4(record.callbackIP)) {
		record.callbackIP = 0;
	}
	if (record.callbackIP == 0) {
		record.udpPort = 0;
		record.kadPort = 0;
	}
	if (record.buddyIP != 0 && !filter.acceptIPv4(record.buddyIP)) {
		record.buddyIP = 0;
	}
	if (record.buddyIPv6 && !filter.acceptIPv6(*record.buddyIPv6)) {
		record.buddyIPv6.reset();
	}
	if (record.buddyPort == 0 || (record.buddyIP == 0 && !record.buddyIPv6)) {
		record.buddyIP = 0;
		record.buddyIPv6.reset();
		record.buddyPort = 0;
	}
}

//! Bit 3 of the connect options. The callback goes to the Kad port and is keyed to the user hash.
inline bool HasDirectCallback(const SRecord &record)
{
	return record.callbackIP != 0 && record.kadPort != 0 && record.userHash && record.connectOptions &&
	       (*record.connectOptions & 0x08) != 0;
}

//! Whether a received record is matched to a known source by its buddy ID. Without a user hash
//! nothing else identifies a Kad LowID: its ID is 1 and it has no server.
inline bool MatchesKnownSourceByBuddyID(const SRecord &record)
{
	return !record.userHash && record.buddyID.has_value();
}

//! Whether a received LowID record names a way to reach the source. A buddy ID without an
//! endpoint is enough: a Kad search near the ID finds the buddy.
inline bool HasLowIDRoute(const SRecord &record)
{
	return (record.serverIP != 0 && record.serverPort != 0) || record.buddyID ||
	       HasDirectCallback(record);
}

//! A LowID that names no route but its own IPv6, like a Kad result without a buddy.
inline bool HasOnlyIPv6Route(const SRecord &record)
{
	return IsLowID(record.hybridID) && record.ipv6 && record.buddyPort == 0 && !HasLowIDRoute(record);
}

//! The Kad rule for a source firewalled on IPv4 with no buddy: dial the IPv6 it names. `app`
//! carries our hello ID and what the build, the settings and the address family allow.
inline bool ShouldDialIPv6(const SRecord &record, SKadSourceFacts app)
{
	if (!HasOnlyIPv6Route(record)) {
		return false;
	}
	app.sourceType = 3;
	app.publishedIPv6 = CNetworkAddress::FromIPv6Bytes(record.ipv6->data());
	app.carriesBuddy = false;
	return ShouldContactKadSourceOverIPv6(app);
}

//! Whether to send one of our LowID sources. The record must already be sanitized and carry
//! only a buddy ID learned from Kad.
inline bool ShouldOfferLowID(const SRecord &record, bool peerSkipsUnknownTags)
{
	const bool buddyRoute = record.buddyID && record.buddyPort != 0;
	// Without the user hash and connect options a peer cannot do the direct callback.
	return buddyRoute || (peerSkipsUnknownTags && HasDirectCallback(record));
}

} // namespace ExtSX

#endif // EXTSOURCEEXCHANGE_H
