// This file is part of the aMule Project.
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Licensed under the GNU General Public License, version 2 or later.

#ifndef SOURCEEXCHANGEPACKET_H
#define SOURCEEXCHANGEPACKET_H

#include "Logger.h"
#include "MemFile.h"
#include "Packet.h"
#include <common/Format.h>
#include <protocol/Protocols.h>
#include <protocol/ed2k/Client2Client/TCP.h>
#include <protocol/ed2k/Constants.h>
#include <algorithm>

// The wire format shared by complete-file and partial-file source replies. Source eligibility
// and the choice of source ID belong to the caller: the two file types use different policies.
class CSourceExchangePacket
{
public:
	CSourceExchangePacket(const CMD4Hash &fileHash,
		bool supportsSX2,
		uint8 sx1Version,
		uint8 requestedVersion,
		uint16 requestedOptions)
	: m_sx2(supportsSX2 && requestedVersion > 0)
	, m_version(m_sx2 ? std::min(requestedVersion, uint8{ SOURCEEXCHANGE2_VERSION }) : sx1Version)
	{
		if (m_sx2) {
			m_data.WriteUInt8(m_version);
			if (requestedOptions != 0) {
				AddDebugLogLineN(logKnownFiles,
					CFormat("Client requested unknown options for SourceExchange2: %u") %
						requestedOptions);
			}
		} else if (supportsSX2) {
			AddDebugLogLineN(logKnownFiles,
				"Client which announced to support SX2 sent SX1 packet instead");
		}
		m_data.WriteHash(fileHash);
		m_data.WriteUInt16(0);
	}

	uint8 GetVersion() const { return m_version; }

	void AddSource(uint32 id,
		uint16 port,
		uint32 serverIP,
		uint16 serverPort,
		const CMD4Hash &userHash,
		uint8 cryptOptions)
	{
		m_data.WriteUInt32(id);
		m_data.WriteUInt16(port);
		m_data.WriteUInt32(serverIP);
		m_data.WriteUInt16(serverPort);
		if (m_version >= 2) {
			m_data.WriteHash(userHash);
		}
		if (m_version >= 4) {
			m_data.WriteUInt8(cryptOptions);
		}
		++m_count;
	}

	// Preserve the existing limit: both callers stopped after writing their 501st source.
	bool IsFull() const { return m_count > 500; }

	// Ownership passes to the caller; an empty reply is not sent.
	CPacket *CreatePacket()
	{
		if (m_count == 0) {
			return nullptr;
		}
		m_data.Seek(m_sx2 ? 17 : 16, wxFromStart);
		m_data.WriteUInt16(m_count);
		CPacket *result =
			new CPacket(m_data, OP_EMULEPROT, m_sx2 ? OP_ANSWERSOURCES2 : OP_ANSWERSOURCES);
		if (result->GetPacketSize() > 354) {
			result->PackPacket();
		}
		return result;
	}

private:
	const bool m_sx2;
	const uint8 m_version;
	CMemFile m_data;
	uint16 m_count = 0;
};

#endif
