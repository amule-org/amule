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
#include "QuicStreamAcceptor.h"

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_gnutls.h>

#include <netinet/in.h>

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

constexpr size_t kMaxUdpPayload = 1452;
constexpr int kMaxFlushPacketsPerCall = 16;

ngtcp2_tstamp NanosecondsFromMs(uint64_t nowMs)
{
	return nowMs * UINT64_C(1000000);
}

// ngtcp2_path_storage_zero() leaves sa_family unset, which crashes ngtcp2's own sockaddr_eq()
// the moment it compares two paths -- confirmed under ASan/UBSan, not assumed; ngtcp2's own test
// suite never actually uses a zeroed family either, despite naming its equivalent "null_path".
// A real, if address-less, AF_INET endpoint is what a "no real path" has to mean here.
void InitZeroPath(ngtcp2_path_storage &path)
{
	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	ngtcp2_path_storage_init(&path, reinterpret_cast<ngtcp2_sockaddr *>(&addr), sizeof(addr),
		reinterpret_cast<ngtcp2_sockaddr *>(&addr), sizeof(addr), nullptr);
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

class CQuicNgtcp2Connection final : public IQuicConnection, public IQuicStreamOperations
{
public:
	CQuicNgtcp2Connection(std::shared_ptr<IQuicNgtcp2Engine> engine,
		std::shared_ptr<IQuicDatagramSink> sink,
		IQuicNgtcp2Engine::Handle handle,
		const CNetworkAddress &address,
		uint16_t port,
		const std::string &issuedCid,
		IQuicStreamAcceptor *acceptor)
	: m_engine(std::move(engine))
	, m_sink(std::move(sink))
	, m_handle(handle)
	, m_address(address)
	, m_port(port)
	, m_issuedCid(issuedCid)
	, m_acceptor(acceptor)
	{
	}

	~CQuicNgtcp2Connection() override { Close(); }

	bool ProcessDatagram(const uint8_t *data, size_t length, uint64_t nowMs) override
	{
		if (m_closed || data == nullptr || length == 0)
			return false;
		m_lastNowMs = nowMs;
		if (!m_engine->Read(m_handle, data, length, nowMs)) {
			Close();
			return false;
		}
		if (!m_engine->Flush(m_handle, *m_sink, m_address, m_port, nowMs)) {
			Close();
			return false;
		}
		OfferStreamIfJustOpened();
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

	std::vector<uint8_t> DrainStreamData() override
	{
		return m_closed ? std::vector<uint8_t>() : m_engine->DrainStreamData(m_handle);
	}

	void ExtendStreamReadWindow(size_t bytes) override
	{
		if (!m_closed) {
			m_engine->ExtendStreamReadWindow(m_handle, bytes);
		}
	}

	void Tick(uint64_t nowMs) override
	{
		if (m_closed) {
			return;
		}
		m_lastNowMs = nowMs;
		if (!m_engine->Tick(m_handle, *m_sink, m_address, m_port, nowMs)) {
			Close();
		}
	}

	// IQuicStreamOperations: CQuicSocketTransport's handle is this connection's own
	// IQuicNgtcp2Engine::Handle, but it is never read back here -- this connection already
	// knows which engine handle and stream it is.
	std::ptrdiff_t WriteStream(IQuicStreamOperations::Handle, const uint8_t *data, size_t length) override
	{
		if (m_closed) {
			return -1;
		}
		return m_engine->WriteStreamData(m_handle, data, length, *m_sink, m_address, m_port, m_lastNowMs);
	}

	void CloseStream(IQuicStreamOperations::Handle) override
	{
		if (!m_closed) {
			// The transport is the one closing itself here -- detach before shutting the stream
			// down, so a byte that arrives in the gap is buffered (DrainStreamData()) rather than
			// delivered to a transport that just asked to stop hearing from this connection.
			m_engine->AttachTransport(m_handle, nullptr);
			m_engine->ShutdownStream(m_handle);
		}
	}

	void ExtendReadWindow(IQuicStreamOperations::Handle, size_t bytes) override { ExtendStreamReadWindow(bytes); }

	void Close() override
	{
		if (m_closed)
			return;
		m_closed = true;
		m_engine->Destroy(m_handle);
		m_handle = nullptr;
	}

private:
	// The point a connection has something worth handing to the rest of aMule: the peer has
	// opened its one stream (QuicSocketTransport.h). Checked after every successful
	// ProcessDatagram(), since that is the only place a stream can newly open.
	void OfferStreamIfJustOpened()
	{
		if (m_streamOffered || m_acceptor == nullptr || !m_engine->HasOpenStream(m_handle)) {
			return;
		}
		m_streamOffered = true;
		auto owned = std::make_unique<CQuicSocketTransport>(*this, m_handle, m_address, m_port, nullptr, true);
		CQuicSocketTransport *raw = owned.get();
		std::unique_ptr<IStreamTransport> transport(std::move(owned));
		if (!m_acceptor->AcceptStream(transport, m_address, m_port)) {
			// Refused: the transport the acceptor declined is still ours to destroy, exactly as
			// CUtpLibraryAdapter does for a refused uTP stream.
			return;
		}
		// Deliver whatever arrived before the transport existed, then everything from here on
		// goes straight to it (see AttachTransport()'s contract).
		const std::vector<uint8_t> buffered = m_engine->DrainStreamData(m_handle);
		if (!buffered.empty()) {
			raw->OnPayload(buffered.data(), buffered.size());
		}
		m_engine->AttachTransport(m_handle, raw);
		// The handshake that had to complete before a stream could even open already proved
		// connectivity; this is the equivalent of CUtpSocketTransport::MarkConnected() for a
		// transport libutp's own state machine would otherwise have marked itself.
		raw->MarkConnected();
	}

	std::shared_ptr<IQuicNgtcp2Engine> m_engine;
	std::shared_ptr<IQuicDatagramSink> m_sink;
	IQuicNgtcp2Engine::Handle m_handle;
	const CNetworkAddress m_address;
	uint16_t m_port;
	std::string m_issuedCid;
	IQuicStreamAcceptor *m_acceptor;
	uint64_t m_lastNowMs = 0;
	bool m_streamOffered = false;
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

	// There is exactly one application stream per connection by design: eD2k always replies on
	// the peer's own stream and never opens one of its own (QuicSocketTransport.h). The first
	// stream id seen, from either callback, is the only one this engine ever tracks; recv_stream
	// data for a later, different stream id is silently ignored rather than accepted into a
	// second stream's place, since nothing above this layer could tell the two apart anyway.
	static int OnStreamOpen(ngtcp2_conn *conn, int64_t streamId, void *userData)
	{
		auto *engine = static_cast<CProductionNgtcp2Engine *>(userData);
		auto it = engine->m_connections.find(conn);
		if (it != engine->m_connections.end() && it->second.streamId < 0) {
			it->second.streamId = streamId;
		}
		return 0;
	}

	static int OnRecvStreamData(ngtcp2_conn *conn,
		uint32_t,
		int64_t streamId,
		uint64_t,
		const uint8_t *data,
		size_t datalen,
		void *userData,
		void *)
	{
		auto *engine = static_cast<CProductionNgtcp2Engine *>(userData);
		auto it = engine->m_connections.find(conn);
		if (it == engine->m_connections.end()) {
			return 0;
		}
		if (it->second.streamId < 0) {
			it->second.streamId = streamId;
		}
		if (it->second.streamId != streamId) {
			return 0;
		}
		// Once a transport is attached (AttachTransport()), bytes go straight to it: buffering
		// them in receivedData too would silently duplicate everything DrainStreamData() already
		// handed to whoever attached the transport in the first place.
		if (it->second.transport != nullptr) {
			it->second.transport->OnPayload(data, datalen);
		} else {
			it->second.receivedData.insert(it->second.receivedData.end(), data, data + datalen);
		}
		return 0;
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
		InitZeroPath(path);

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
		callbacks.stream_open = OnStreamOpen;
		callbacks.recv_stream_data = OnRecvStreamData;

		ngtcp2_settings settings = {};
		ngtcp2_settings_default(&settings);
		// Flush() writes into a kMaxUdpPayload-sized stack buffer; keep ngtcp2 from ever trying
		// to hand it back a larger packet, which PMTUD could otherwise grow towards over time.
		settings.max_tx_udp_payload_size = kMaxUdpPayload;

		ngtcp2_transport_params params = {};
		ngtcp2_transport_params_default(&params);
		params.max_udp_payload_size = kMaxUdpPayload;
		params.original_dcid = originalDcid;
		params.original_dcid_present = 1;
		// Only the remote-initiated windows matter: eD2k always replies on the peer's own
		// stream, never opens one of its own (see QuicSocketTransport.h). ExtendReadWindow()
		// reopens these as the application drains its read buffer, once a real stream exists.
		params.initial_max_stream_data_bidi_remote = CQuicSocketTransport::kReadWindow;
		params.initial_max_stream_data_uni = CQuicSocketTransport::kReadWindow;
		params.initial_max_data = CQuicSocketTransport::kReadWindow;
		// ngtcp2_transport_params_default() leaves this at 0, meaning "the peer may open no
		// bidirectional stream at all". Exactly one is the design (QuicSocketTransport.h): the
		// peer's own stream, never a second.
		params.initial_max_streams_bidi = 1;

		ngtcp2_conn *conn = nullptr;
		if (ngtcp2_conn_server_new(
			    &conn, &dcid, &scid, &path.path, metadata.version, &callbacks, &settings, &params, nullptr, this) != 0 ||
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

	bool Read(Handle handle, const uint8_t *data, size_t length, uint64_t nowMs) override
	{
		auto it = m_connections.find(handle);
		if (it == m_connections.end()) {
			return false;
		}
		ngtcp2_path_storage path;
		InitZeroPath(path);
		// RFC 9000 never requires this engine to know the peer's real address to process a
		// packet correctly: both endpoints of the path are left zero on every call, so ngtcp2
		// never observes a path change and never attempts connection migration. The actual
		// peer address this connection talks to is CQuicNgtcp2Connection's own m_address/m_port,
		// used when flushing output -- not anything ngtcp2 derives from this path.
		return ngtcp2_conn_read_pkt(it->second.conn, &path.path, nullptr, data, length,
			       NanosecondsFromMs(nowMs)) == 0;
	}

	bool Flush(Handle handle,
		IQuicDatagramSink &sink,
		const CNetworkAddress &address,
		uint16_t port,
		uint64_t nowMs) override
	{
		auto it = m_connections.find(handle);
		if (it == m_connections.end()) {
			return false;
		}
		return FlushConnection(it->second, sink, address, port, NanosecondsFromMs(nowMs));
	}

	bool Tick(Handle handle,
		IQuicDatagramSink &sink,
		const CNetworkAddress &address,
		uint16_t port,
		uint64_t nowMs) override
	{
		auto it = m_connections.find(handle);
		if (it == m_connections.end()) {
			return false;
		}
		const ngtcp2_tstamp ts = NanosecondsFromMs(nowMs);
		// "Nothing due yet" is the overwhelmingly common case on every tick: UINT64_MAX means no
		// timer is armed at all, and an unexpired one is simply not this tick's problem.
		if (ngtcp2_conn_get_expiry(it->second.conn) > ts) {
			return true;
		}
		// Fatal here means the timer handling itself concluded the connection is over (RFC 9000
		// section 10.1: the idle timeout elapsed) -- not that a send failed, which FlushConnection()
		// below still reports on its own.
		if (ngtcp2_conn_handle_expiry(it->second.conn, ts) != 0) {
			return false;
		}
		return FlushConnection(it->second, sink, address, port, ts);
	}

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

	std::vector<uint8_t> DrainStreamData(Handle handle) override
	{
		auto it = m_connections.find(handle);
		if (it == m_connections.end()) {
			return {};
		}
		std::vector<uint8_t> data;
		data.swap(it->second.receivedData);
		return data;
	}

	void ExtendStreamReadWindow(Handle handle, size_t bytes) override
	{
		auto it = m_connections.find(handle);
		if (it == m_connections.end() || it->second.streamId < 0) {
			return;
		}
		ngtcp2_conn_extend_max_stream_offset(it->second.conn, it->second.streamId, bytes);
		ngtcp2_conn_extend_max_offset(it->second.conn, bytes);
	}

	bool HasOpenStream(Handle handle) const override
	{
		auto it = m_connections.find(handle);
		return it != m_connections.end() && it->second.streamId >= 0;
	}

	void AttachTransport(Handle handle, CQuicSocketTransport *transport) override
	{
		auto it = m_connections.find(handle);
		if (it != m_connections.end()) {
			it->second.transport = transport;
		}
	}

	std::ptrdiff_t WriteStreamData(Handle handle,
		const uint8_t *data,
		size_t length,
		IQuicDatagramSink &sink,
		const CNetworkAddress &address,
		uint16_t port,
		uint64_t nowMs) override
	{
		auto it = m_connections.find(handle);
		if (it == m_connections.end() || it->second.streamId < 0) {
			return -1;
		}
		const ngtcp2_tstamp ts = NanosecondsFromMs(nowMs);
		uint8_t buf[kMaxUdpPayload];
		ngtcp2_path_storage path;
		InitZeroPath(path);
		ngtcp2_pkt_info pi = {};
		const ngtcp2_vec vec{ const_cast<uint8_t *>(data), length };
		ngtcp2_ssize dataLen = 0;
		// ngtcp2_conn_writev_stream() both accepts stream data and may produce a packet in the
		// same call: there is no separate "queue it for later" step to split this into, unlike
		// CUtpSocketTransport's push model over libutp's own internal timer.
		const ngtcp2_ssize written = ngtcp2_conn_writev_stream(it->second.conn, &path.path, &pi, buf,
			sizeof(buf), &dataLen, NGTCP2_WRITE_STREAM_FLAG_NONE, it->second.streamId, &vec, 1, ts);
		if (written < 0) {
			return -1;
		}
		if (written > 0 && !sink.SendDatagram(buf, static_cast<size_t>(written), address, port)) {
			return -1;
		}
		return dataLen < 0 ? 0 : static_cast<std::ptrdiff_t>(dataLen);
	}

	void ShutdownStream(Handle handle) override
	{
		auto it = m_connections.find(handle);
		if (it != m_connections.end() && it->second.streamId >= 0) {
			ngtcp2_conn_shutdown_stream(it->second.conn, 0, it->second.streamId, 0);
		}
	}

	void Destroy(Handle handle) override
	{
		auto it = m_connections.find(handle);
		if (it != m_connections.end()) {
			// A transport can still be attached if the connection ends on its own (e.g. a
			// protocol error, or the peer closing) rather than through the transport's own
			// Close(): tell it the stream it was built on is gone before the handle it holds
			// becomes dangling.
			if (it->second.transport != nullptr) {
				it->second.transport->OnEnded();
			}
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
		int64_t streamId = -1;
		std::vector<uint8_t> receivedData;
		//! Non-owning: set by AttachTransport() once the caller has handed the stream off. The
		//! caller is responsible for keeping it alive and for calling AttachTransport(handle,
		//! nullptr) before the transport is destroyed.
		CQuicSocketTransport *transport = nullptr;
	};

	// Shared by Flush() (an inbound datagram may need an immediate reply) and Tick() (a timer
	// firing with nothing freshly received still needs to retransmit or probe): both end in
	// exactly the same bounded send loop.
	bool FlushConnection(ConnectionInfo &connection,
		IQuicDatagramSink &sink,
		const CNetworkAddress &address,
		uint16_t port,
		ngtcp2_tstamp ts)
	{
		// A bounded number of packets per flush, not an unbounded drain-until-0 loop: a
		// connection that always has more to send (e.g. a peer that never acknowledges) must
		// not be able to make one flush call monopolize this thread indefinitely.
		for (int round = 0; round < kMaxFlushPacketsPerCall; ++round) {
			uint8_t buf[kMaxUdpPayload];
			ngtcp2_path_storage path;
			InitZeroPath(path);
			ngtcp2_pkt_info pi = {};
			// stream_id -1, no data: this call only flushes what ngtcp2 itself needs to send
			// (handshake CRYPTO frames, ACKs, retransmissions, probes). Application data goes
			// out through WriteStreamData() instead, which can itself produce a packet in the
			// same call.
			const ngtcp2_ssize written = ngtcp2_conn_writev_stream(connection.conn, &path.path, &pi,
				buf, sizeof(buf), nullptr, NGTCP2_WRITE_STREAM_FLAG_NONE, -1, nullptr, 0, ts);
			if (written < 0) {
				return false;
			}
			if (written == 0) {
				return true;
			}
			if (!sink.SendDatagram(buf, static_cast<size_t>(written), address, port)) {
				return false;
			}
		}
		return true;
	}

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

void CQuicNgtcp2Factory::SetAcceptor(IQuicStreamAcceptor *acceptor)
{
	m_acceptor = acceptor;
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
	return std::make_unique<CQuicNgtcp2Connection>(
		m_engine, m_sink, handle, address, port, issuedCid, m_acceptor);
}

std::shared_ptr<IQuicNgtcp2Engine> CreateProductionQuicNgtcp2Engine()
{
	return std::make_shared<CProductionNgtcp2Engine>();
}
