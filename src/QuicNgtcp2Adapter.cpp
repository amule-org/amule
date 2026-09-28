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

#include "QuicNgtcp2Adapter.h"

#include <ngtcp2/ngtcp2.h>

#include <utility>

namespace
{
// ngtcp2_accept() owns the RFC 9000 server acceptance rules (Initial type, datagram of at
// least 1200 bytes, token-less destination CID of at least 8 bytes, empty source CID allowed).
// It only flags a clear fixed bit; section 17.2 requires discarding it, because grease_quic_bit
// cannot have been negotiated before the first Initial.
bool ParseInitialMetadata(const uint8_t *data, size_t length, CQuicInitialMetadata &metadata)
{
	if (data == nullptr || length == 0)
		return false;
	ngtcp2_pkt_hd header;
	if (ngtcp2_accept(&header, data, length) != 0 || header.version != NGTCP2_PROTO_VER_V1 ||
		(header.flags & NGTCP2_PKT_FLAG_FIXED_BIT_CLEAR) != 0)
		return false;
	metadata.version = header.version;
	metadata.destinationCid.assign(reinterpret_cast<const char *>(header.dcid.data), header.dcid.datalen);
	metadata.sourceCid.assign(reinterpret_cast<const char *>(header.scid.data), header.scid.datalen);
	return true;
}

class CQuicNgtcp2Connection final : public IQuicConnection
{
public:
	CQuicNgtcp2Connection(std::shared_ptr<IQuicNgtcp2Engine> engine,
		std::shared_ptr<IQuicDatagramSink> sink,
		IQuicNgtcp2Engine::Handle handle,
		const CNetworkAddress &address,
		uint16_t port)
	: m_engine(std::move(engine))
	, m_sink(std::move(sink))
	, m_handle(handle)
	, m_address(address)
	, m_port(port)
	{
	}

	~CQuicNgtcp2Connection() override { Close(); }

	bool ProcessDatagram(const uint8_t *data, size_t length) override
	{
		if (m_closed || data == nullptr || length == 0)
			return false;
		if (!m_engine->Read(m_handle, data, length)) {
			Close();
			return false;
		}
		if (!m_engine->Flush(m_handle, *m_sink, m_address, m_port)) {
			Close();
			return false;
		}
		return true;
	}

	bool IsClosed() const override { return m_closed; }

	bool OwnsConnectionId(const std::string &cid) const override
	{
		return !m_closed && m_engine->OwnsConnectionId(m_handle, cid);
	}

	void Close() override
	{
		if (m_closed)
			return;
		m_closed = true;
		m_engine->Destroy(m_handle);
		m_handle = nullptr;
	}

private:
	std::shared_ptr<IQuicNgtcp2Engine> m_engine;
	std::shared_ptr<IQuicDatagramSink> m_sink;
	IQuicNgtcp2Engine::Handle m_handle;
	const CNetworkAddress m_address;
	uint16_t m_port;
	bool m_closed = false;
};

class CFailClosedNgtcp2Engine final : public IQuicNgtcp2Engine
{
public:
	Handle CreateServer(const CQuicTlsPolicy &,
		const CNetworkAddress &,
		uint16_t,
		const CQuicInitialMetadata &) override
	{
		return nullptr;
	}
	bool Read(Handle, const uint8_t *, size_t) override { return false; }
	bool Flush(Handle, IQuicDatagramSink &, const CNetworkAddress &, uint16_t) override { return false; }
	bool OwnsConnectionId(Handle, const std::string &) const override { return false; }
	void Destroy(Handle) override {}
};
} // namespace

CQuicNgtcp2Factory::CQuicNgtcp2Factory(const CQuicTlsPolicy &policy,
	std::shared_ptr<IQuicDatagramSink> sink,
	std::shared_ptr<IQuicNgtcp2Engine> engine)
: m_policy(policy)
, m_sink(std::move(sink))
, m_engine(std::move(engine))
{
}

std::unique_ptr<IQuicConnection> CQuicNgtcp2Factory::CreateInbound(const uint8_t *data,
	size_t length,
	const CNetworkAddress &address,
	uint16_t port,
	const std::string &cid)
{
	CQuicInitialMetadata metadata;
	if (m_policy.session == nullptr || m_policy.credentials == nullptr || m_policy.verifier == nullptr ||
		m_policy.ngtcp2Session == nullptr ||
		m_policy.ngtcp2Session->NativeGnuTlsSession() == nullptr || m_sink == nullptr ||
		m_engine == nullptr || cid.empty() || cid.size() > 20 ||
		!ParseInitialMetadata(data, length, metadata) || metadata.destinationCid != cid) {
		return nullptr;
	}
	IQuicNgtcp2Engine::Handle handle = m_engine->CreateServer(m_policy, address, port, metadata);
	if (handle == nullptr)
		return nullptr;
	return std::make_unique<CQuicNgtcp2Connection>(m_engine, m_sink, handle, address, port);
}

std::shared_ptr<IQuicNgtcp2Engine> CreateProductionQuicNgtcp2Engine()
{
	return std::make_shared<CFailClosedNgtcp2Engine>();
}
