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

#include "QuicGnuTlsSession.h"
#include "QuicNattProtocol.h"
#include "QuicNgtcp2Adapter.h"
#include "QuicSocketTransport.h"

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_gnutls.h>

#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <utility>
#include <vector>

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

bool AssignCid(ngtcp2_cid &cid, const std::string &value)
{
	if (value.size() > NGTCP2_MAX_CIDLEN) {
		return false;
	}
	cid.datalen = value.size();
	std::memcpy(cid.data, value.data(), cid.datalen);
	return true;
}

void Ngtcp2Random(uint8_t *dest, size_t destlen, const ngtcp2_rand_ctx *)
{
	std::random_device random;
	for (size_t i = 0; i < destlen; ++i) {
		dest[i] = static_cast<uint8_t>(random());
	}
}

int Ngtcp2NewConnectionId(ngtcp2_conn *, ngtcp2_cid *cid, uint8_t *token, size_t cidlen, void *)
{
	if (cidlen > NGTCP2_MAX_CIDLEN) {
		return NGTCP2_ERR_CALLBACK_FAILURE;
	}
	cid->datalen = cidlen;
	Ngtcp2Random(cid->data, cid->datalen, nullptr);
	Ngtcp2Random(token, NGTCP2_STATELESS_RESET_TOKENLEN, nullptr);
	return 0;
}

// Every connection needs a distinct server-issued SCID: a fixed value would collide across
// concurrent peers and break the OwnsConnectionId() uniqueness this engine promises.
std::string RandomServerCid()
{
	uint8_t bytes[8];
	Ngtcp2Random(bytes, sizeof(bytes), nullptr);
	return std::string(reinterpret_cast<const char *>(bytes), sizeof(bytes));
}

// Bridges ngtcp2_crypto_conn_ref back to the ngtcp2_conn that owns it: user_data is simply that
// conn, set once right after ngtcp2_conn_server_new() succeeds. GnuTLS's handshake/secret
// functions (installed by ngtcp2_crypto_gnutls_configure_server_session()) use this to reach the
// connection they are driving, given only the gnutls_session_t.
ngtcp2_conn *GetConnFromRef(ngtcp2_crypto_conn_ref *ref)
{
	return static_cast<ngtcp2_conn *>(ref->user_data);
}

class CQuicNgtcp2Connection final : public IQuicConnection
{
public:
	CQuicNgtcp2Connection(std::shared_ptr<IQuicNgtcp2Engine> engine,
		std::shared_ptr<IQuicDatagramSink> sink,
		IQuicNgtcp2Engine::Handle handle,
		const CNetworkAddress &address,
		uint16_t port,
		const std::string &issuedCid)
	: m_engine(std::move(engine))
	, m_sink(std::move(sink))
	, m_handle(handle)
	, m_address(address)
	, m_port(port)
	, m_issuedCid(issuedCid)
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

	std::string GetIssuedConnectionId() const override
	{
		return m_closed ? std::string() : m_issuedCid;
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
	std::string m_issuedCid;
	bool m_closed = false;
};

class CProductionNgtcp2Engine final : public IQuicNgtcp2Engine
{
public:
	~CProductionNgtcp2Engine() override
	{
		for (auto &connection : m_connections) {
			ngtcp2_conn_del(connection.second.conn);
		}
	}

	Handle CreateServer(const CQuicTlsPolicy &policy,
		const CNetworkAddress &,
		uint16_t,
		const CQuicInitialMetadata &metadata) override
	{
		// Each connection gets its own GnuTLS session: a gnutls_session_t holds one handshake's
		// worth of state and cannot be shared across connections. policy.credentials -- a
		// certificate and key -- is the only part of the policy actually read here, and is safe
		// to share across every connection this engine ever creates.
		if (policy.credentials == nullptr) {
			return nullptr;
		}
		auto tlsSession = std::make_unique<CQuicGnuTlsSession>();
		const auto *alpn = reinterpret_cast<const uint8_t *>(QuicNatt::QUIC_NATT_ALPN);
		const size_t alpnLength = sizeof(QuicNatt::QUIC_NATT_ALPN) - 1;
		if (!tlsSession->ConfigureTls13Alpn(*policy.credentials, policy.verifier, alpn, alpnLength, true)) {
			return nullptr;
		}
		if (ngtcp2_crypto_gnutls_configure_server_session(tlsSession->NativeGnuTlsSession()) != 0) {
			return nullptr;
		}

		ngtcp2_cid dcid = {};
		if (!AssignCid(dcid, metadata.sourceCid)) {
			return nullptr;
		}

		ngtcp2_cid originalDcid = {};
		if (!AssignCid(originalDcid, metadata.destinationCid)) {
			return nullptr;
		}

		ngtcp2_cid scid = {};
		if (!AssignCid(scid, RandomServerCid())) {
			return nullptr;
		}

		ngtcp2_path_storage path;
		ngtcp2_path_storage_zero(&path);

		// ngtcp2's own GnuTLS crypto helpers replace the fail-closed stand-ins this engine used
		// before a real TLS session existed: they drive the actual TLS 1.3 handshake, key
		// installation and AEAD operations through the session configured above.
		ngtcp2_callbacks callbacks = {};
		callbacks.recv_client_initial = ngtcp2_crypto_recv_client_initial_cb;
		callbacks.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
		callbacks.encrypt = ngtcp2_crypto_encrypt_cb;
		callbacks.decrypt = ngtcp2_crypto_decrypt_cb;
		callbacks.hp_mask = ngtcp2_crypto_hp_mask_cb;
		callbacks.rand = Ngtcp2Random;
		callbacks.get_new_connection_id = Ngtcp2NewConnectionId;
		callbacks.update_key = ngtcp2_crypto_update_key_cb;
		callbacks.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
		callbacks.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
		callbacks.get_path_challenge_data = ngtcp2_crypto_get_path_challenge_data_cb;

		ngtcp2_settings settings = {};
		ngtcp2_settings_default(&settings);

		ngtcp2_transport_params params = {};
		ngtcp2_transport_params_default(&params);
		params.original_dcid = originalDcid;
		params.original_dcid_present = 1;
		// Only the remote-initiated windows matter: eD2k always replies on the peer's own
		// stream, never opens one of its own (see QuicSocketTransport.h). ExtendReadWindow()
		// reopens these as the application drains its read buffer, once a real stream exists.
		params.initial_max_stream_data_bidi_remote = CQuicSocketTransport::kReadWindow;
		params.initial_max_stream_data_uni = CQuicSocketTransport::kReadWindow;
		params.initial_max_data = CQuicSocketTransport::kReadWindow;

		ngtcp2_conn *conn = nullptr;
		if (ngtcp2_conn_server_new(
			    &conn, &dcid, &scid, &path.path, metadata.version, &callbacks, &settings, &params, nullptr, nullptr) != 0 ||
			conn == nullptr) {
			return nullptr;
		}
		ngtcp2_conn_set_tls_native_handle(conn, tlsSession->NativeGnuTlsSession());

		Handle handle = conn;
		ConnectionInfo info;
		info.conn = conn;
		info.issuedCid.assign(reinterpret_cast<const char *>(scid.data), scid.datalen);
		info.tlsSession = std::move(tlsSession);
		info.connRef.get_conn = GetConnFromRef;
		info.connRef.user_data = conn;
		auto inserted = m_connections.emplace(handle, std::move(info));
		// gnutls_session_set_ptr() needs the ref's final, stable address: the map node, not the
		// local `info` this function already moved from.
		gnutls_session_set_ptr(
			inserted.first->second.tlsSession->NativeGnuTlsSession(), &inserted.first->second.connRef);
		return handle;
	}

	bool Read(Handle, const uint8_t *, size_t) override { return false; }
	bool Flush(Handle, IQuicDatagramSink &, const CNetworkAddress &, uint16_t) override { return false; }

	bool OwnsConnectionId(Handle handle, const std::string &cid) const override
	{
		auto it = m_connections.find(handle);
		if (it == m_connections.end()) {
			return false;
		}

		const size_t count = ngtcp2_conn_get_scid(it->second.conn, nullptr);
		std::vector<ngtcp2_cid> scids(count);
		ngtcp2_conn_get_scid(it->second.conn, scids.data());
		for (const ngtcp2_cid &scid : scids) {
			if (cid.size() == scid.datalen && std::memcmp(cid.data(), scid.data, scid.datalen) == 0) {
				return true;
			}
		}
		return false;
	}

	std::string GetIssuedConnectionId(Handle handle) const override
	{
		auto it = m_connections.find(handle);
		return it == m_connections.end() ? std::string() : it->second.issuedCid;
	}

	uint64_t GetAdvertisedReadWindow(Handle handle) const override
	{
		auto it = m_connections.find(handle);
		if (it == m_connections.end()) {
			return 0;
		}
		return ngtcp2_conn_get_local_transport_params(it->second.conn)->initial_max_data;
	}

	void Destroy(Handle handle) override
	{
		auto it = m_connections.find(handle);
		if (it != m_connections.end()) {
			ngtcp2_conn_del(it->second.conn);
			m_connections.erase(it);
		}
	}

private:
	struct ConnectionInfo
	{
		ngtcp2_conn *conn = nullptr;
		std::string issuedCid;
		std::unique_ptr<CQuicGnuTlsSession> tlsSession;
		ngtcp2_crypto_conn_ref connRef{};
	};

	std::map<Handle, ConnectionInfo> m_connections;
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
	std::string issuedCid = m_engine->GetIssuedConnectionId(handle);
	return std::make_unique<CQuicNgtcp2Connection>(m_engine, m_sink, handle, address, port, issuedCid);
}

std::shared_ptr<IQuicNgtcp2Engine> CreateProductionQuicNgtcp2Engine()
{
	return std::make_shared<CProductionNgtcp2Engine>();
}
