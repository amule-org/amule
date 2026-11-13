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

#include <muleunit/test.h>

#include "ExtSourceExchange.h"
#include "MemFile.h"
#include "SourceExchangePacket.h"
#include "kademlia/utils/UInt128.h"

#include <algorithm>
#include <memory>
#include <vector>

using namespace muleunit;

namespace
{
const ExtSX::Bytes16 WireBuddyID = {
	0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f
};
const ExtSX::Bytes16 PublicIPv6 = { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };

std::vector<uint8> Contents(const CMemFile &file)
{
	return std::vector<uint8>(file.GetRawBuffer(), file.GetRawBuffer() + file.GetLength());
}

void Append(std::vector<uint8> &out, std::initializer_list<uint8> bytes)
{
	out.insert(out.end(), bytes);
}

void AppendHashTag(std::vector<uint8> &out, uint8 id, const ExtSX::Bytes16 &hash)
{
	Append(out, { 0x81, id });
	out.insert(out.end(), hash.begin(), hash.end());
}

ExtSX::SRecord Decode(const std::vector<uint8> &bytes)
{
	CMemFile data(bytes.data(), bytes.size());
	ExtSX::SRecord record = ExtSX::ReadRecord(data);
	ASSERT_EQUALS(sint64{ 0 }, data.GetAvailable());
	return record;
}

std::vector<uint8> Encode(const ExtSX::SRecord &record, bool peerSkipsUnknownTags)
{
	CMemFile data;
	ExtSX::WriteRecord(data, record, peerSkipsUnknownTags);
	return Contents(data);
}

std::vector<uint8> TagIDs(const std::vector<uint8> &record)
{
	CMemFile data(record.data(), record.size());
	data.ReadUInt32();
	data.ReadUInt16();
	std::vector<uint8> ids(data.ReadUInt8());
	for (uint8 &id : ids) {
		id = CTag(data, true).GetNameID();
	}
	return ids;
}

bool Contains(const std::vector<uint8> &ids, uint8 id)
{
	return std::find(ids.begin(), ids.end(), id) != ids.end();
}

// LowID 0x123456 on TCP 4662, as eMuleAI's WriteExtendedSourceExchangeData writes it: callback
// address 1.2.3.4, Kad 4672 / UDP 4665, buddy 5.6.7.8:4672, buddy ID, server 9.10.11.12:4661.
std::vector<uint8> EmuleAILowIDRecord()
{
	std::vector<uint8> out = { 0x00, 0x12, 0x34, 0x56, 0x36, 0x12, 7 };
	Append(out, { 0x83, CT_EMULE_ADDRESS, 1, 2, 3, 4 });
	Append(out, { 0x83, CT_EMULE_UDPPORTS, 0x39, 0x12, 0x40, 0x12 });
	Append(out, { 0x83, CT_EMULE_BUDDYIP, 5, 6, 7, 8 });
	Append(out, { 0x88, CT_EMULE_BUDDYUDP, 0x40, 0x12 });
	AppendHashTag(out, CT_EMULE_SERVINGBUDDYID, WireBuddyID);
	Append(out, { 0x83, CT_EMULE_SERVERIP, 9, 10, 11, 12 });
	Append(out, { 0x88, CT_EMULE_SERVERTCP, 0x35, 0x12 });
	return out;
}

ExtSX::SAddressFilter RejectOnly(uint32 badIPv4)
{
	ExtSX::SAddressFilter filter;
	filter.acceptIPv4 = [badIPv4](uint32 ip) { return ip != badIPv4; };
	filter.acceptIPv6 = [](const ExtSX::Bytes16 &ip) { return ip[0] == 0x20; };
	return filter;
}

ExtSX::SRecord LowIDWithBuddy()
{
	ExtSX::SRecord record;
	record.hybridID = 0x123456;
	record.port = 4662;
	record.buddyIP = 0x08070605;
	record.buddyPort = 4672;
	record.buddyID = WireBuddyID;
	return record;
}
} // namespace

DECLARE_SIMPLE(ExtSourceExchange)

TEST(ExtSourceExchange, DecodesAnEmuleAIRecord)
{
	const ExtSX::SRecord record = Decode(EmuleAILowIDRecord());
	ASSERT_EQUALS(uint32{ 0x123456 }, record.hybridID);
	ASSERT_EQUALS(uint16{ 4662 }, record.port);
	ASSERT_EQUALS(uint32{ 0x04030201 }, record.callbackIP);
	ASSERT_EQUALS(uint16{ 4672 }, record.kadPort);
	ASSERT_EQUALS(uint16{ 4665 }, record.udpPort);
	ASSERT_EQUALS(uint32{ 0x08070605 }, record.buddyIP);
	ASSERT_EQUALS(uint16{ 4672 }, record.buddyPort);
	ASSERT_EQUALS(uint32{ 0x0C0B0A09 }, record.serverIP);
	ASSERT_EQUALS(uint16{ 4661 }, record.serverPort);
	ASSERT_TRUE(record.buddyID.has_value());
	// Unlike TAG_BUDDYHASH, 0xBF is not converted: it is already in ToByteArray order.
	ASSERT_TRUE(*record.buddyID == WireBuddyID);
	ASSERT_FALSE(record.userHash.has_value());
	ASSERT_FALSE(record.connectOptions.has_value());
}

// A peer without bit 5 gets the same bytes eMuleAI would write for the same source.
TEST(ExtSourceExchange, WritesTheEmuleAIRecordByteForByte)
{
	const std::vector<uint8> wire = EmuleAILowIDRecord();
	ASSERT_TRUE(Encode(Decode(wire), false) == wire);
}

// eMuleAI and emule-qt put the ToByteArray bytes in 0xBF, and send the callback request as
// WriteUInt128(CUInt128(id)), which reverses each 4-byte group:
//   eMuleAI  DownloadQueue.cpp KademliaSearchFile: pServingBuddyID->ToByteArray(cID);
//            BaseClient.cpp: CTag(CT_EMULE_SERVINGBUDDYID, GetServingBuddyID());
//            BaseClient.cpp: bio.WriteUInt128(Kademlia::CUInt128(GetServingBuddyID()));
//   emule-qt KadSearch.cpp buddyHashFromTagString; KnownFile.cpp: (CT_EMULE_SERVINGBUDDYID, src->buddyID());
//            UpDownClient.cpp sendKadCallback: kad::io::writeUInt128(data, kad::UInt128(m_buddyID));
TEST(ExtSourceExchange, KadCallbackSendsTheBuddyIDTheWayEmuleAIAndEmuleQtDo)
{
	const ExtSX::Bytes16 fromRecord = Decode(EmuleAILowIDRecord()).buddyID.value();
	ASSERT_TRUE(fromRecord == WireBuddyID);

	// As CUpDownClient::TryToConnect writes it for a buddy ID read from 0xBF.
	CMemFile request;
	request.WriteUInt128(Kademlia::CUInt128(fromRecord.data()));
	const std::vector<uint8> sent = Contents(request);
	const std::vector<uint8> expected = {
		0x13, 0x12, 0x11, 0x10, 0x17, 0x16, 0x15, 0x14, 0x1b, 0x1a, 0x19, 0x18, 0x1f, 0x1e, 0x1d, 0x1c
	};
	ASSERT_TRUE(sent == expected);
}

TEST(ExtSourceExchange, WritesTheBuddyIDUnchanged)
{
	CMemFile data;
	ExtSX::WriteRecord(data, LowIDWithBuddy(), false);
	data.Seek(0, wxFromStart);
	data.ReadUInt32();
	data.ReadUInt16();
	const uint8 count = data.ReadUInt8();
	bool found = false;
	for (uint8 i = 0; i < count; ++i) {
		const CTag tag(data, true);
		if (tag.GetNameID() == CT_EMULE_SERVINGBUDDYID) {
			found = tag.GetHash() == CMD4Hash(WireBuddyID.data());
		}
	}
	ASSERT_TRUE(found);
}

TEST(ExtSourceExchange, DecodesAnEmuleQtAnswerAndSkipsItsBuddyOffer)
{
	ExtSX::Bytes16 userHash;
	userHash.fill(0xab);

	// HighID 1.2.3.4 with server, IPv6, user hash and connect options, then the answering
	// peer's own record carrying the buddy offer 0xBD and an unknown string-named tag.
	std::vector<uint8> answer = { 1, 2, 3, 4, 0x36, 0x12, 5 };
	Append(answer, { 0x83, CT_EMULE_SERVERIP, 9, 10, 11, 12 });
	Append(answer, { 0x88, CT_EMULE_SERVERTCP, 0x35, 0x12 });
	AppendHashTag(answer, CT_MOD_IP_V6, PublicIPv6);
	AppendHashTag(answer, CT_EMULE_USERHASH, userHash);
	Append(answer, { 0x89, CT_EMULE_CONOPTS, 0x03 });
	Append(answer, { 4, 3, 2, 1, 0x37, 0x12, 2 });
	Append(answer, { 0x88, 0xBD, 0x40, 0x12 });
	Append(answer, { 0x02, 2, 0, 'z', 'z', 3, 0, 'a', 'b', 'c' });

	CMemFile data(answer.data(), answer.size());
	const ExtSX::SRecord source = ExtSX::ReadRecord(data);
	const ExtSX::SRecord answerer = ExtSX::ReadRecord(data);
	ASSERT_EQUALS(sint64{ 0 }, data.GetAvailable());

	ASSERT_EQUALS(uint32{ 0x01020304 }, source.hybridID);
	ASSERT_FALSE(IsLowID(source.hybridID));
	ASSERT_EQUALS(uint32{ 0x0C0B0A09 }, source.serverIP);
	ASSERT_EQUALS(uint16{ 4661 }, source.serverPort);
	ASSERT_TRUE(source.ipv6 && *source.ipv6 == PublicIPv6);
	ASSERT_TRUE(source.userHash && *source.userHash == CMD4Hash(userHash.data()));
	ASSERT_TRUE(source.connectOptions && *source.connectOptions == 0x03);

	ASSERT_EQUALS(uint32{ 0x04030201 }, answerer.hybridID);
	ASSERT_EQUALS(uint16{ 4663 }, answerer.port);
	ASSERT_EQUALS(uint32{ 0 }, answerer.buddyIP);
	ASSERT_EQUALS(uint16{ 0 }, answerer.buddyPort);
	ASSERT_EQUALS(uint16{ 0 }, answerer.kadPort);
}

TEST(ExtSourceExchange, DecodesAnEmuleQtLowIDRoute)
{
	ExtSX::Bytes16 userHash;
	userHash.fill(0xcd);
	std::vector<uint8> wire = { 0x00, 0x00, 0x00, 0x01, 0x36, 0x12, 8 };
	Append(wire, { 0x83, CT_EMULE_ADDRESS, 1, 2, 3, 4 });
	Append(wire, { 0x83, CT_EMULE_UDPPORTS, 0x39, 0x12, 0x40, 0x12 });
	Append(wire, { 0x83, CT_EMULE_BUDDYIP, 5, 6, 7, 8 });
	AppendHashTag(wire, CT_EMULE_SERVINGBUDDYIPV6, PublicIPv6);
	Append(wire, { 0x88, CT_EMULE_BUDDYUDP, 0x40, 0x12 });
	AppendHashTag(wire, CT_EMULE_SERVINGBUDDYID, WireBuddyID);
	AppendHashTag(wire, CT_EMULE_USERHASH, userHash);
	Append(wire, { 0x89, CT_EMULE_CONOPTS, 0x09 });

	ExtSX::SRecord record = Decode(wire);
	ExtSX::SanitizeRoute(record, RejectOnly(0));
	ASSERT_EQUALS(uint32{ 1 }, record.hybridID);
	ASSERT_TRUE(record.buddyIPv6 && *record.buddyIPv6 == PublicIPv6);
	ASSERT_TRUE(record.buddyID && *record.buddyID == WireBuddyID);
	ASSERT_TRUE(ExtSX::HasDirectCallback(record));
	ASSERT_TRUE(ExtSX::HasLowIDRoute(record));
}

// eMuleAI rejects a wrong-typed known tag; aMule skips it like an unknown one.
TEST(ExtSourceExchange, IgnoresKnownTagsOfTheWrongType)
{
	std::vector<uint8> wire = { 0x00, 0x12, 0x34, 0x56, 0x36, 0x12, 2 };
	Append(wire, { 0x83, CT_EMULE_SERVINGBUDDYID, 1, 2, 3, 4 });
	AppendHashTag(wire, CT_EMULE_BUDDYIP, WireBuddyID);
	const ExtSX::SRecord record = Decode(wire);
	ASSERT_FALSE(record.buddyID.has_value());
	ASSERT_EQUALS(uint32{ 0 }, record.buddyIP);
}

TEST(ExtSourceExchange, TruncatedRecordThrows)
{
	std::vector<uint8> wire = EmuleAILowIDRecord();
	wire.resize(wire.size() - 1);
	CMemFile data(wire.data(), wire.size());
	ASSERT_RAISES(CSafeIOException, ExtSX::ReadRecord(data));
}

// eMuleAI drops the rest of the answer on any tag it does not know (eMuleAI/eMuleAI#171). Every
// combination of fields, for a LowID and a HighID: without bit 5 only its nine tags go out.
TEST(ExtSourceExchange, PeerWithoutBit5NeverGetsATagOutsideEmuleAIsSet)
{
	const unsigned fieldCount = 12;
	for (const uint32 id : { uint32{ 0x123456 }, uint32{ 0x01020304 } }) {
		for (unsigned mask = 0; mask < (1u << fieldCount); ++mask) {
			ExtSX::SRecord record;
			record.hybridID = id;
			record.port = 4662;
			const auto has = [mask](unsigned bit) { return (mask & (1u << bit)) != 0; };
			record.serverIP = has(0) ? 0x0C0B0A09 : 0;
			record.serverPort = has(0) ? 4661 : 0;
			if (has(1)) {
				record.ipv6 = PublicIPv6;
			}
			if (has(2)) {
				record.userHash = CMD4Hash(WireBuddyID.data());
			}
			if (has(3)) {
				record.connectOptions = 0x0f;
			}
			record.callbackIP = has(4) ? 0x04030201 : 0;
			record.udpPort = has(5) ? 4665 : 0;
			record.kadPort = has(6) ? 4672 : 0;
			record.buddyIP = has(7) ? 0x08070605 : 0;
			if (has(8)) {
				record.buddyIPv6 = PublicIPv6;
			}
			record.buddyPort = has(9) ? 4672 : 0;
			if (has(10)) {
				record.buddyID = WireBuddyID;
			}
			if (has(11)) {
				ExtSX::SanitizeRoute(record, RejectOnly(0x08070605));
			}

			const std::vector<uint8> legacy = TagIDs(Encode(record, false));
			for (const uint8 tag : legacy) {
				ASSERT_TRUE(ExtSX::IsTagSafeForEveryReader(tag));
			}
			const std::vector<uint8> extended = TagIDs(Encode(record, true));
			ASSERT_EQUALS(record.userHash.has_value(), Contains(extended, CT_EMULE_USERHASH));
			ASSERT_EQUALS(
				record.connectOptions.has_value(), Contains(extended, CT_EMULE_CONOPTS));
			ASSERT_EQUALS(legacy.size() + record.userHash.has_value() +
					      record.connectOptions.has_value(),
				extended.size());
		}
	}
}

TEST(ExtSourceExchange, NineTagsAreSafeForEveryReader)
{
	unsigned safe = 0;
	for (unsigned id = 0; id < 256; ++id) {
		safe += ExtSX::IsTagSafeForEveryReader(static_cast<uint8>(id)) ? 1 : 0;
	}
	ASSERT_EQUALS(9u, safe);
	ASSERT_FALSE(ExtSX::IsTagSafeForEveryReader(CT_EMULE_USERHASH));
	ASSERT_FALSE(ExtSX::IsTagSafeForEveryReader(CT_EMULE_CONOPTS));
	ASSERT_FALSE(ExtSX::IsTagSafeForEveryReader(0xBD));
}

TEST(ExtSourceExchange, RoundTripsEveryFieldToAPeerWithBit5)
{
	ExtSX::SRecord record = LowIDWithBuddy();
	record.serverIP = 0x0C0B0A09;
	record.serverPort = 4661;
	record.ipv6 = PublicIPv6;
	record.userHash = CMD4Hash(WireBuddyID.data());
	record.connectOptions = 0x09;
	record.callbackIP = 0x04030201;
	record.udpPort = 4665;
	record.kadPort = 4672;
	record.buddyIPv6 = PublicIPv6;

	const ExtSX::SRecord back = Decode(Encode(record, true));
	ASSERT_EQUALS(record.hybridID, back.hybridID);
	ASSERT_EQUALS(record.port, back.port);
	ASSERT_EQUALS(record.serverIP, back.serverIP);
	ASSERT_EQUALS(record.serverPort, back.serverPort);
	ASSERT_TRUE(back.ipv6 == record.ipv6);
	ASSERT_TRUE(back.userHash == record.userHash);
	ASSERT_TRUE(back.connectOptions == record.connectOptions);
	ASSERT_EQUALS(record.callbackIP, back.callbackIP);
	ASSERT_EQUALS(record.udpPort, back.udpPort);
	ASSERT_EQUALS(record.kadPort, back.kadPort);
	ASSERT_EQUALS(record.buddyIP, back.buddyIP);
	ASSERT_TRUE(back.buddyIPv6 == record.buddyIPv6);
	ASSERT_EQUALS(record.buddyPort, back.buddyPort);
	ASSERT_TRUE(back.buddyID == record.buddyID);
}

TEST(ExtSourceExchange, WritesTheIDAsHtonlOfTheHybridID)
{
	ExtSX::SRecord record;
	record.hybridID = 0x01020304;
	std::vector<uint8> wire = Encode(record, true);
	ASSERT_TRUE(std::vector<uint8>({ 1, 2, 3, 4, 0, 0, 0 }) == wire);
	// A HighID ending in .0 stays a HighID.
	record.hybridID = 0x01020300;
	ASSERT_EQUALS(uint32{ 0x01020300 }, Decode(Encode(record, true)).hybridID);
}

TEST(ExtSourceExchange, HighIDRouteTagsAreIgnored)
{
	std::vector<uint8> wire = EmuleAILowIDRecord();
	wire[0] = 1; // now HighID 1.18.52.86
	ExtSX::SRecord record = Decode(wire);
	ExtSX::SanitizeRoute(record, RejectOnly(0));
	ASSERT_EQUALS(uint32{ 0 }, record.callbackIP);
	ASSERT_EQUALS(uint16{ 0 }, record.kadPort);
	ASSERT_EQUALS(uint32{ 0 }, record.buddyIP);
	ASSERT_EQUALS(uint16{ 0 }, record.buddyPort);
	ASSERT_FALSE(record.buddyID.has_value());
	ASSERT_EQUALS(uint32{ 0x0C0B0A09 }, record.serverIP);
}

TEST(ExtSourceExchange, DropsRejectedAddresses)
{
	ExtSX::SRecord record = Decode(EmuleAILowIDRecord());
	ExtSX::SanitizeRoute(record, RejectOnly(0x04030201));
	ASSERT_EQUALS(uint32{ 0 }, record.callbackIP);
	ASSERT_EQUALS(uint16{ 0 }, record.udpPort);
	ASSERT_EQUALS(uint16{ 0 }, record.kadPort);
	ASSERT_EQUALS(uint32{ 0x08070605 }, record.buddyIP);

	record = Decode(EmuleAILowIDRecord());
	ExtSX::SanitizeRoute(record, RejectOnly(0x08070605));
	ASSERT_EQUALS(uint32{ 0 }, record.buddyIP);
	ASSERT_EQUALS(uint16{ 0 }, record.buddyPort);
	// The ID still lets a Kad search find the buddy.
	ASSERT_TRUE(record.buddyID.has_value());
	ASSERT_TRUE(ExtSX::HasLowIDRoute(record));

	record = LowIDWithBuddy();
	record.ipv6 = ExtSX::Bytes16{};
	record.buddyIPv6 = ExtSX::Bytes16{};
	ExtSX::SanitizeRoute(record, RejectOnly(0x08070605));
	ASSERT_FALSE(record.ipv6.has_value());
	ASSERT_FALSE(record.buddyIPv6.has_value());
	ASSERT_EQUALS(uint16{ 0 }, record.buddyPort);

	record = LowIDWithBuddy();
	record.buddyIPv6 = PublicIPv6;
	ExtSX::SanitizeRoute(record, RejectOnly(0x08070605));
	ASSERT_TRUE(record.buddyIPv6.has_value());
	ASSERT_EQUALS(uint16{ 4672 }, record.buddyPort);
}

TEST(ExtSourceExchange, BuddyEndpointWithoutAPortIsDroppedWhole)
{
	ExtSX::SRecord record = LowIDWithBuddy();
	record.buddyIPv6 = PublicIPv6;
	record.buddyPort = 0;
	ExtSX::SanitizeRoute(record, RejectOnly(0));
	ASSERT_EQUALS(uint32{ 0 }, record.buddyIP);
	ASSERT_FALSE(record.buddyIPv6.has_value());

	record = LowIDWithBuddy();
	record.buddyIP = 0;
	ExtSX::SanitizeRoute(record, RejectOnly(0));
	ASSERT_EQUALS(uint16{ 0 }, record.buddyPort);
}

TEST(ExtSourceExchange, LowIDNeedsARoute)
{
	ExtSX::SRecord record;
	record.hybridID = 1;
	ASSERT_FALSE(ExtSX::HasLowIDRoute(record));
	record.buddyIP = 0x08070605;
	record.buddyPort = 4672;
	ASSERT_FALSE(ExtSX::HasLowIDRoute(record));
	record.serverIP = 0x0C0B0A09;
	record.serverPort = 4661;
	ASSERT_TRUE(ExtSX::HasLowIDRoute(record));

	record = ExtSX::SRecord{};
	record.hybridID = 1;
	record.callbackIP = 0x04030201;
	record.kadPort = 4672;
	record.connectOptions = 0x08;
	ASSERT_FALSE(ExtSX::HasDirectCallback(record)); // the callback is keyed to the user hash
	record.userHash = CMD4Hash(WireBuddyID.data());
	ASSERT_TRUE(ExtSX::HasDirectCallback(record));
	record.connectOptions = 0x07;
	ASSERT_FALSE(ExtSX::HasDirectCallback(record));
}

TEST(ExtSourceExchange, OffersALowIDOnlyWithARoute)
{
	ExtSX::SRecord record = LowIDWithBuddy();
	ASSERT_TRUE(ExtSX::ShouldOfferLowID(record, false));
	ASSERT_TRUE(ExtSX::ShouldOfferLowID(record, true));

	// An ID the buddy cannot be reached without, or an endpoint without the ID, is no route.
	record.buddyPort = 0;
	ASSERT_FALSE(ExtSX::ShouldOfferLowID(record, true));
	record = LowIDWithBuddy();
	record.buddyID.reset();
	ASSERT_FALSE(ExtSX::ShouldOfferLowID(record, true));

	// The direct callback needs the user hash and connect options, which only bit 5 peers get.
	record.callbackIP = 0x04030201;
	record.kadPort = 4672;
	record.userHash = CMD4Hash(WireBuddyID.data());
	record.connectOptions = 0x08;
	ASSERT_FALSE(ExtSX::ShouldOfferLowID(record, false));
	ASSERT_TRUE(ExtSX::ShouldOfferLowID(record, true));
}

TEST(ExtSourceExchange, AnswerUsesVersionOneWhateverWasRequested)
{
	for (const uint8 requested : { uint8{ 1 }, uint8{ 4 }, uint8{ 255 } }) {
		CSourceExchangePacket packet(CMD4Hash(WireBuddyID.data()), true, 4, requested, 0, true);
		ASSERT_TRUE(packet.IsExtended());
		ASSERT_EQUALS(uint8{ SOURCEEXCHANGEEXT_VERSION }, packet.GetVersion());
		packet.AddExtendedSource(LowIDWithBuddy(), false);
		std::unique_ptr<CPacket> sent(packet.CreatePacket());
		ASSERT_TRUE(sent != nullptr);
		ASSERT_EQUALS(static_cast<uint8>(OP_ANSWERSOURCES2), sent->GetOpCode());

		CMemFile data(sent->GetDataBuffer(), sent->GetPacketSize());
		ASSERT_EQUALS(uint8{ SOURCEEXCHANGEEXT_VERSION }, data.ReadUInt8());
		ASSERT_TRUE(data.ReadHash() == CMD4Hash(WireBuddyID.data()));
		ASSERT_EQUALS(uint16{ 1 }, data.ReadUInt16());
		const ExtSX::SRecord record = ExtSX::ReadRecord(data);
		ASSERT_EQUALS(sint64{ 0 }, data.GetAvailable());
		ASSERT_TRUE(record.buddyID == LowIDWithBuddy().buddyID);
	}
}

TEST(ExtSourceExchange, ExtendedNeedsSX2)
{
	CSourceExchangePacket sx1Request(CMD4Hash(), true, 4, 0, 0, true);
	ASSERT_FALSE(sx1Request.IsExtended());
	CSourceExchangePacket noSX2(CMD4Hash(), false, 4, 1, 0, true);
	ASSERT_FALSE(noSX2.IsExtended());
	CSourceExchangePacket classic(CMD4Hash(), true, 4, 1, 0, false);
	ASSERT_FALSE(classic.IsExtended());
	ASSERT_EQUALS(uint8{ 1 }, classic.GetVersion());
}

namespace
{
// 2001:4860:4860::8888: globally routable, unlike the documentation prefix above.
const ExtSX::Bytes16 RoutableIPv6 = {
	0x20, 0x01, 0x48, 0x60, 0x48, 0x60, 0, 0, 0, 0, 0, 0, 0, 0, 0x88, 0x88
};

ExtSX::SRecord IPv6OnlyLowID()
{
	ExtSX::SRecord record;
	record.hybridID = 1;
	record.port = 4662;
	record.ipv6 = RoutableIPv6;
	return record;
}

// Every switch an IPv6 dial needs is on, as in KadSourceAddressPolicyTest.
SKadSourceFacts DialableApp(uint32 ourHelloID)
{
	SKadSourceFacts app;
	app.ourHelloID = ourHelloID;
	app.ipv6Built = true;
	app.familyPermitted = true;
	return app;
}
} // namespace

// 0xAE is one of eMuleAI's tags, so the source's own IPv6 goes to every ExtSX peer.
TEST(ExtSourceExchange, SendsTheSourcesIPv6ToEveryExtendedPeer)
{
	for (const uint32 id : { uint32{ 0x123456 }, uint32{ 0x01020304 } }) {
		ExtSX::SRecord record;
		record.hybridID = id;
		record.ipv6 = RoutableIPv6;
		ExtSX::SanitizeRoute(record, RejectOnly(0));
		ASSERT_TRUE(Contains(TagIDs(Encode(record, false)), CT_MOD_IP_V6));
		ASSERT_TRUE(Contains(TagIDs(Encode(record, true)), CT_MOD_IP_V6));
		ASSERT_TRUE(Decode(Encode(record, false)).ipv6 == RoutableIPv6);
	}
}

TEST(ExtSourceExchange, IPv6IsTheOnlyRouteOnlyWithoutAnyOther)
{
	ASSERT_TRUE(ExtSX::HasOnlyIPv6Route(IPv6OnlyLowID()));

	ExtSX::SRecord record = IPv6OnlyLowID();
	record.ipv6.reset();
	ASSERT_FALSE(ExtSX::HasOnlyIPv6Route(record));

	record = IPv6OnlyLowID();
	record.hybridID = 0x01020304;
	ASSERT_FALSE(ExtSX::HasOnlyIPv6Route(record));

	record = IPv6OnlyLowID();
	record.buddyID = WireBuddyID;
	ASSERT_FALSE(ExtSX::HasOnlyIPv6Route(record));

	// A buddy endpoint is a buddy, as TAG_SERVERIP/TAG_SERVERPORT are in a Kad result.
	record = IPv6OnlyLowID();
	record.buddyIP = 0x08070605;
	record.buddyPort = 4672;
	ASSERT_FALSE(ExtSX::HasOnlyIPv6Route(record));

	record = IPv6OnlyLowID();
	record.serverIP = 0x0C0B0A09;
	record.serverPort = 4661;
	ASSERT_FALSE(ExtSX::HasOnlyIPv6Route(record));

	record = IPv6OnlyLowID();
	record.callbackIP = 0x04030201;
	record.kadPort = 4672;
	record.userHash = CMD4Hash(WireBuddyID.data());
	record.connectOptions = 0x08;
	ASSERT_FALSE(ExtSX::HasOnlyIPv6Route(record));
}

// Kad's buddy-less rule: any nonzero ID of ours, a routable address, and a dial the build and the
// settings allow.
TEST(ExtSourceExchange, DialsAnIPv6OnlyLowIDLikeABuddylessKadSource)
{
	for (const uint32 ourHelloID : { 1u, 0x00ABCDEFu, 0xC0A80001u }) {
		ASSERT_TRUE(ExtSX::ShouldDialIPv6(IPv6OnlyLowID(), DialableApp(ourHelloID)));
	}
	ASSERT_FALSE(ExtSX::ShouldDialIPv6(IPv6OnlyLowID(), DialableApp(0)));

	SKadSourceFacts app = DialableApp(1);
	app.ipv6Built = false;
	ASSERT_FALSE(ExtSX::ShouldDialIPv6(IPv6OnlyLowID(), app));
	app = DialableApp(1);
	app.familyPermitted = false;
	ASSERT_FALSE(ExtSX::ShouldDialIPv6(IPv6OnlyLowID(), app));
	app = DialableApp(1);
	app.proxyEnabled = true;
	ASSERT_FALSE(ExtSX::ShouldDialIPv6(IPv6OnlyLowID(), app));

	// A third party's address must name a peer across hosts.
	ExtSX::SRecord record = IPv6OnlyLowID();
	ExtSX::Bytes16 loopback{};
	loopback[15] = 1;
	record.ipv6 = loopback;
	ASSERT_FALSE(ExtSX::ShouldDialIPv6(record, DialableApp(1)));
	record.ipv6 = PublicIPv6; // documentation prefix
	ASSERT_FALSE(ExtSX::ShouldDialIPv6(record, DialableApp(1)));
}

// A HighID keeps its IPv4 dial; its IPv6 is never dialled.
TEST(ExtSourceExchange, HighIDIsNotDialledOverIPv6)
{
	ExtSX::SRecord record = IPv6OnlyLowID();
	record.hybridID = 0x01020304;
	ASSERT_FALSE(ExtSX::ShouldDialIPv6(record, DialableApp(1)));
}

// What emule-qt sends for a LowID with an open IPv6 and nothing else.
TEST(ExtSourceExchange, DecodesAnEmuleQtIPv6OnlyLowID)
{
	std::vector<uint8> wire = { 0x00, 0x00, 0x00, 0x01, 0x36, 0x12, 1 };
	AppendHashTag(wire, CT_MOD_IP_V6, RoutableIPv6);
	ExtSX::SRecord record = Decode(wire);
	ExtSX::SanitizeRoute(record, RejectOnly(0));
	ASSERT_FALSE(ExtSX::HasLowIDRoute(record));
	ASSERT_TRUE(ExtSX::ShouldDialIPv6(record, DialableApp(1)));
}

// A Kad LowID has ID 1 and no server: without a user hash only its buddy ID names it.
TEST(ExtSourceExchange, AHashlessRecordIsMatchedByItsBuddyID)
{
	ExtSX::SRecord record;
	record.hybridID = 1;
	record.buddyID = WireBuddyID;
	ASSERT_TRUE(ExtSX::MatchesKnownSourceByBuddyID(record));
}

TEST(ExtSourceExchange, ARecordWithAUserHashIsMatchedByItsHash)
{
	ExtSX::SRecord record;
	record.hybridID = 1;
	record.buddyID = WireBuddyID;
	record.userHash = CMD4Hash(PublicIPv6.data());
	ASSERT_FALSE(ExtSX::MatchesKnownSourceByBuddyID(record));
}

TEST(ExtSourceExchange, ARecordWithoutABuddyIDIsNotMatchedByOne)
{
	ExtSX::SRecord record;
	record.hybridID = 0x1235;
	record.serverIP = 0x0100007F;
	record.serverPort = 4661;
	ASSERT_FALSE(ExtSX::MatchesKnownSourceByBuddyID(record));
}
