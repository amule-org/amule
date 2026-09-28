//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//
// Any parts of this program contributed by third-party developers are copyrighted
// by their respective authors.
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

#ifndef QUICNGTCP2ADAPTER_H
#define QUICNGTCP2ADAPTER_H

#include "QuicContext.h"
#include "QuicLibraryAdapter.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

struct CQuicInitialMetadata
{
	uint32_t version;
	std::string destinationCid;
	std::string sourceCid;
};

class IQuicDatagramSink
{
public:
	virtual ~IQuicDatagramSink() = default;
	virtual bool SendDatagram(const uint8_t *, size_t, const CNetworkAddress &, uint16_t) = 0;
};

class IQuicNgtcp2Engine
{
public:
	using Handle = void *;
	virtual ~IQuicNgtcp2Engine() = default;
	virtual Handle CreateServer(
		const CQuicTlsPolicy &, const CNetworkAddress &, uint16_t, const CQuicInitialMetadata &) = 0;
	virtual bool Read(Handle, const uint8_t *, size_t) = 0;
	virtual bool Flush(Handle, IQuicDatagramSink &, const CNetworkAddress &, uint16_t) = 0;
	//! Backed by ngtcp2_conn_get_scid(): the source connection IDs issued and not retired.
	virtual bool OwnsConnectionId(Handle, const std::string &) const = 0;
	virtual void Destroy(Handle) = 0;
};

class CQuicNgtcp2Factory final : public IQuicConnectionFactory
{
public:
	CQuicNgtcp2Factory(const CQuicTlsPolicy &,
		std::shared_ptr<IQuicDatagramSink>,
		std::shared_ptr<IQuicNgtcp2Engine>);
	std::unique_ptr<IQuicConnection> CreateInbound(
		const uint8_t *, size_t, const CNetworkAddress &, uint16_t, const std::string &) override;

private:
	CQuicTlsPolicy m_policy;
	std::shared_ptr<IQuicDatagramSink> m_sink;
	std::shared_ptr<IQuicNgtcp2Engine> m_engine;
};

// The production dependency is intentionally unavailable until the ngtcp2/TLS
// event-loop bridge is wired. It always fails closed; tests inject an engine.
std::shared_ptr<IQuicNgtcp2Engine> CreateProductionQuicNgtcp2Engine();

#endif
