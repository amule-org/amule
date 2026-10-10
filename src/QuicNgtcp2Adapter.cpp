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

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <functional>
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
// got3nks' review on #1710 (finding #3, High): without these, handle_expiry() never ends a
// connection on its own, and spoofed Initials could fill every admission slot permanently.
constexpr uint64_t kHandshakeTimeoutMs = 10000; // 10s: generous even over a slow real path.
constexpr uint64_t kIdleTimeoutMs = 60000;      // 60s of silence after that is a dead peer.

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
	ngtcp2_sockaddr_in addr = {};
	addr.sin_family = NGTCP2_AF_INET;
	ngtcp2_path_storage_init(&path,
		reinterpret_cast<ngtcp2_sockaddr *>(&addr),
		sizeof(addr),
		reinterpret_cast<ngtcp2_sockaddr *>(&addr),
		sizeof(addr),
		nullptr);
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
		IQuicStreamAcceptor *acceptor,
		const std::array<uint8_t, 16> &localIdentity,
		std::function<uint64_t()> transportClock)
	: m_engine(std::move(engine))
	, m_sink(std::move(sink))
	, m_handle(handle)
	, m_address(address)
	, m_port(port)
	, m_issuedCid(issuedCid)
	, m_acceptor(acceptor)
	, m_localIdentity(localIdentity)
	, m_transportClock(std::move(transportClock))
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
		EndConnectionIfStreamEnded(nowMs);
		if (!m_closed) {
			m_engine->NotifyWritable(m_handle);
		}
		return true;
	}

	bool IsClosed() const override { return m_closed; }

	bool OwnsConnectionId(const std::string &cid) const override
	{
		return !m_closed && m_engine->OwnsConnectionId(m_handle, cid);
	}

	std::string GetIssuedConnectionId() const override { return m_closed ? std::string() : m_issuedCid; }

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
			return;
		}
		// A pending proof write deferred by pacing/congestion (TryExchangeEaqn1Proof()) has no
		// transport yet for IQuicNgtcp2Engine::NotifyWritable() to reach -- this tick, the same
		// moment that congestion easing or a loss-detection retransmit would otherwise be
		// noticed, is this connection's own equivalent retry point.
		OfferStreamIfJustOpened();
		EndConnectionIfStreamEnded(nowMs);
		if (!m_closed) {
			m_engine->NotifyWritable(m_handle);
		}
	}

	// IQuicStreamOperations: CQuicSocketTransport's handle is this connection's own
	// IQuicNgtcp2Engine::Handle, but it is never read back here -- this connection already
	// knows which engine handle and stream it is. @p nowMs is CQuicSocketTransport's own injected
	// clock, read fresh in Flush(), not m_lastNowMs: this can run long after the last datagram/
	// tick this connection actually saw (the application writing on its own schedule), and
	// ngtcp2_conn_writev_stream()'s pacing/RTT math needs the real time of this call.
	std::ptrdiff_t WriteStream(
		IQuicStreamOperations::Handle, const uint8_t *data, size_t length, uint64_t nowMs) override
	{
		if (m_closed) {
			return -1;
		}
		return m_engine->WriteStreamData(m_handle, data, length, *m_sink, m_address, m_port, nowMs);
	}

	//! @p nowMs: see WriteStream().
	void CloseStream(IQuicStreamOperations::Handle, uint64_t nowMs) override
	{
		if (!m_closed) {
			// The transport is the one closing itself here -- detach before shutting the stream
			// down, so a byte that arrives in the gap is buffered (DrainStreamData()) rather than
			// delivered to a transport that just asked to stop hearing from this connection.
			m_engine->AttachTransport(m_handle, nullptr);
			// Graceful, not ShutdownStream()'s reset: a transport-initiated close is always a
			// voluntary one (CQuicSocketTransport::Close(), itself already having had its own
			// chance to flush anything queued) -- there is nothing here that justifies discarding
			// whatever the engine has already accepted and is still waiting to confirm.
			m_engine->CloseStreamGracefully(m_handle, *m_sink, m_address, m_port, nowMs);
		}
	}

	void AbortStream(IQuicStreamOperations::Handle) override
	{
		if (!m_closed) {
			// Unlike CloseStream(): a write that failed outright means nothing about this stream
			// can be trusted enough to try preserving, so this resets it instead of asking for a
			// graceful FIN.
			m_engine->AttachTransport(m_handle, nullptr);
			m_engine->ShutdownStream(m_handle);
		}
	}

	void ExtendReadWindow(IQuicStreamOperations::Handle, size_t bytes) override
	{
		ExtendStreamReadWindow(bytes);
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
	// The point a connection has something worth handing to the rest of aMule: the peer has
	// opened its one stream (QuicSocketTransport.h). Checked after every successful
	// ProcessDatagram(), since that is the only place a stream can newly open.
	void OfferStreamIfJustOpened()
	{
		if (m_streamOffered || m_acceptor == nullptr || !m_engine->HasOpenStream(m_handle)) {
			return;
		}
		if (!m_proofExchanged && !TryExchangeEaqn1Proof()) {
			return;
		}
		m_streamOffered = true;
		auto owned = std::make_unique<CQuicSocketTransport>(
			*this, m_handle, m_address, m_port, nullptr, true, m_transportClock);
		CQuicSocketTransport *raw = owned.get();
		std::unique_ptr<IStreamTransport> transport(std::move(owned));
		if (!m_acceptor->AcceptStream(transport, m_address, m_port)) {
			// Refused: the transport the acceptor declined is still ours to destroy, exactly as
			// CUtpLibraryAdapter does for a refused uTP stream.
			return;
		}
		// Deliver whatever arrived after the proof (held back from the stream buffer while this
		// connection was still waiting on TryExchangeEaqn1Proof()), then everything from here on
		// goes straight to it (see AttachTransport()'s contract).
		if (!m_proofBuffer.empty()) {
			raw->OnPayload(m_proofBuffer.data(), m_proofBuffer.size());
			m_proofBuffer.clear();
		}
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

	//! eMuleAI sends EAQN1 + its hash + the target hash it is calling (QuicNattProtocol.h,
	//! NgTcp2GnuTlsBridge.cpp:711-828) as the first bytes on the stream it opens, and expects
	//! ours back before it treats the socket as connected. Without this, a real eMuleAI peer's
	//! proof would reach CClientTCPSocket as ordinary eD2k data, and no proof of ours would ever
	//! be sent -- the peer never completes its own side of the hand-off.
	//!
	//! Returns true once both sides of the exchange are done and the connection is ready to be
	//! handed to the acceptor; false to keep waiting, either for more of the peer's proof or for
	//! ours (m_ourProof) to finish going out. eMuleAI coalesces its Finished and its proof into one
	//! datagram, so our reply is written at the same ts as the handshake-completion flush and
	//! ngtcp2 pacing can defer it (0 bytes accepted). That is "not yet", never a failure: the write
	//! is retried from ProcessDatagram()/Tick(). Only a failed validation or a fatal write error
	//! closes the connection.
	bool TryExchangeEaqn1Proof()
	{
		if (!m_peerProofValidated) {
			const std::vector<uint8_t> drained = m_engine->DrainStreamData(m_handle);
			m_proofBuffer.insert(m_proofBuffer.end(), drained.begin(), drained.end());
			if (m_proofBuffer.size() < QuicNatt::EAQN1_PROOF_SIZE) {
				return false;
			}
			// nullptr: this connection is always the inbound (server) side, and there is no
			// prior rendezvous context here that already told us who ought to be calling -- any
			// peer whose target hash matches ours (or is still zero, a first contact) is
			// admitted.
			if (!QuicNatt::ValidateEaqn1Proof(
				    m_proofBuffer.data(), m_proofBuffer.size(), m_localIdentity, nullptr)) {
				Close();
				return false;
			}
			std::array<uint8_t, 16> peerHash{};
			std::copy(m_proofBuffer.begin() + 5, m_proofBuffer.begin() + 21, peerHash.begin());
			const auto proof = QuicNatt::BuildEaqn1Proof(m_localIdentity, &peerHash);
			m_ourProof.assign(proof.begin(), proof.end());
			m_proofBuffer.erase(
				m_proofBuffer.begin(), m_proofBuffer.begin() + QuicNatt::EAQN1_PROOF_SIZE);
			m_peerProofValidated = true;
		}
		if (!m_ourProof.empty()) {
			// The same send path as ordinary stream data: this is the one and only write ever
			// queued ahead of the stream being offered (OfferStreamIfJustOpened() only offers
			// it once this returns true), so there is nothing else this could race ahead of or
			// fall behind on the wire.
			const std::ptrdiff_t written = m_engine->WriteStreamData(m_handle,
				m_ourProof.data(),
				m_ourProof.size(),
				*m_sink,
				m_address,
				m_port,
				m_lastNowMs);
			if (written < 0) {
				// A genuine fatal connection error (not flow-control/congestion/pacing, which
				// WriteStreamData() already reports as 0 -- see above): fail closed, same as
				// every other RFC 9000 admission check this engine already applies.
				Close();
				return false;
			}
			if (written > 0) {
				m_ourProof.erase(m_ourProof.begin(), m_ourProof.begin() + written);
			}
			if (!m_ourProof.empty()) {
				return false;
			}
		}
		m_proofExchanged = true;
		return true;
	}

	//! With exactly one stream per connection by design (QuicSocketTransport.h), that stream
	//! ending -- cleanly (stream_close) or by reset (stream_reset), from either side -- leaves
	//! nothing left to use this connection for. Without this, a connection whose stream ended
	//! stays alive doing nothing until CQuicContext::kIdleTimeoutMs finally reclaims it: every
	//! finished eD2k session would hold one of CQuicContext::kMaxConnectionsPerScope's slots for
	//! up to a minute after it had nothing left to do.
	void EndConnectionIfStreamEnded(uint64_t nowMs)
	{
		if (!m_closed && m_engine->IsStreamEnded(m_handle)) {
			m_engine->EndConnection(m_handle, *m_sink, m_address, m_port, nowMs);
			m_closed = true;
			m_handle = nullptr;
		}
	}

	std::shared_ptr<IQuicNgtcp2Engine> m_engine;
	std::shared_ptr<IQuicDatagramSink> m_sink;
	IQuicNgtcp2Engine::Handle m_handle;
	const CNetworkAddress m_address;
	uint16_t m_port;
	std::string m_issuedCid;
	IQuicStreamAcceptor *m_acceptor;
	const std::array<uint8_t, 16> m_localIdentity;
	std::function<uint64_t()> m_transportClock;
	//! Accumulated until it reaches EAQN1_PROOF_SIZE: a peer's proof can in principle arrive
	//! split across more than one recv_stream_data() delivery, and nothing shorter than the
	//! full proof is safe to validate.
	std::vector<uint8_t> m_proofBuffer;
	bool m_peerProofValidated = false;
	//! The remainder of our own proof still waiting to go out, trimmed from the front as
	//! WriteStreamData() accepts bytes of it -- empty both before the peer's proof validates and
	//! once ours has fully gone out (m_proofExchanged distinguishes the two).
	std::vector<uint8_t> m_ourProof;
	bool m_proofExchanged = false;
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
		uint32_t flags,
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
		// The peer will send no more on this stream -- IsStreamEnded() picks this up the same way
		// it does stream_close/stream_reset below, from the earliest signal rather than waiting on
		// confirmation that both sides are fully done.
		if (flags & NGTCP2_STREAM_DATA_FLAG_FIN) {
			it->second.streamEnded = true;
		}

		return 0;
	}

	// stream_close is not called if the connection itself closes first -- irrelevant here, since
	// that only happens once this same streamEnded flag has already made EndConnection() do
	// exactly that.
	static int OnStreamClose(
		ngtcp2_conn *conn, uint32_t, int64_t streamId, uint64_t, void *userData, void *)
	{
		auto *engine = static_cast<CProductionNgtcp2Engine *>(userData);
		auto it = engine->m_connections.find(conn);
		if (it != engine->m_connections.end() && it->second.streamId == streamId) {
			it->second.streamEnded = true;
		}
		return 0;
	}

	static int OnStreamReset(
		ngtcp2_conn *conn, int64_t streamId, uint64_t, uint64_t, void *userData, void *)
	{
		auto *engine = static_cast<CProductionNgtcp2Engine *>(userData);
		auto it = engine->m_connections.find(conn);
		if (it != engine->m_connections.end() && it->second.streamId == streamId) {
			it->second.streamEnded = true;
		}
		return 0;
	}

	//! See ConnectionInfo::unackedSendChunks: this is what makes it safe for WriteStreamData()'s
	//! caller to free its own copy the instant that call returns -- it erases our copy instead,
	//! once (and only once) ngtcp2 will never need to read it again.
	static int OnAckedStreamData(
		ngtcp2_conn *conn, int64_t streamId, uint64_t, uint64_t datalen, void *userData, void *)
	{
		auto *engine = static_cast<CProductionNgtcp2Engine *>(userData);
		auto it = engine->m_connections.find(conn);
		if (it == engine->m_connections.end() || it->second.streamId != streamId) {
			return 0;
		}
		ConnectionInfo &info = it->second;
		// Sequential, non-overlapping, gap-free per the ngtcp2 contract, but not necessarily
		// aligned to our own chunk boundaries (one acked_stream_data_offset() call can cover
		// several chunks, or only part of one): walk the front, consuming as we go, and only ever
		// pop_front() a chunk once it is acknowledged in full. A chunk still owes bytes ngtcp2
		// might read again for retransmission right up until that point, so its buffer must never
		// move or shrink before then.
		size_t remaining = datalen;
		while (remaining > 0 && !info.unackedSendChunks.empty()) {
			const size_t frontSize = info.unackedSendChunks.front().size();
			const size_t available = frontSize - info.frontChunkAcked;
			const size_t consumed = std::min(remaining, available);
			info.frontChunkAcked += consumed;
			remaining -= consumed;
			info.unackedSendBytes -= consumed;
			if (info.frontChunkAcked == frontSize) {
				info.unackedSendChunks.pop_front();
				info.frontChunkAcked = 0;
			}
		}
		return 0;
	}

	Handle CreateServer(const CQuicTlsPolicy &policy,
		const CNetworkAddress &,
		uint16_t,
		const CQuicInitialMetadata &metadata,
		uint64_t nowMs) override
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
		if (!tlsSession->ConfigureTls13Alpn(*policy.credentials, nullptr, alpn, alpnLength, true)) {
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
		callbacks.acked_stream_data_offset = OnAckedStreamData;
		callbacks.stream_close = OnStreamClose;
		callbacks.stream_reset = OnStreamReset;

		ngtcp2_settings settings = {};
		ngtcp2_settings_default(&settings);
		// Flush() writes into a kMaxUdpPayload-sized stack buffer; keep ngtcp2 from ever trying
		// to hand it back a larger packet, which PMTUD could otherwise grow towards over time.
		settings.max_tx_udp_payload_size = kMaxUdpPayload;
		// ngtcp2_settings_default() leaves this at UINT64_MAX (no timeout): the deadline is
		// initial_ts + handshake_timeout, so initial_ts must be this connection's own creation
		// time, not whatever ngtcp2_settings_default() left there (0) -- relative to that, a
		// 10s deadline would already be in the past for any connection created after this
		// process's first few seconds of life, failing its very next Tick().
		settings.initial_ts = NanosecondsFromMs(nowMs);
		settings.handshake_timeout = NanosecondsFromMs(kHandshakeTimeoutMs);

		ngtcp2_transport_params params = {};
		ngtcp2_transport_params_default(&params);
		params.max_udp_payload_size = kMaxUdpPayload;
		// ngtcp2_transport_params_default() leaves this at 0 (disabled): handle_expiry() would
		// then never end a quiescent connection on its own. Spoofed Initials cost nothing to
		// send; without this, 8 per scope (CQuicContext::kMaxConnectionsPerScope) times every
		// scope needed to fill 256 total slots never frees up again.
		params.max_idle_timeout = NanosecondsFromMs(kIdleTimeoutMs);
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
		if (ngtcp2_conn_server_new(&conn,
			    &dcid,
			    &scid,
			    &path.path,
			    metadata.version,
			    &callbacks,
			    &settings,
			    &params,
			    nullptr,
			    this) != 0 ||
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
		gnutls_session_set_ptr(inserted.first->second.tlsSession->NativeGnuTlsSession(),
			&inserted.first->second.connRef);
		return handle;
	}

	bool Read(Handle handle, const uint8_t *data, size_t length, uint64_t nowMs) override
	{
		auto it = m_connections.find(handle);
		if (it == m_connections.end()) {
			return false;
		}
		const ngtcp2_tstamp ts = NanosecondsFromMs(nowMs);
		CloseBurstBefore(it->second, ts);
		ngtcp2_path_storage path;
		InitZeroPath(path);
		// RFC 9000 never requires this engine to know the peer's real address to process a
		// packet correctly: both endpoints of the path are left zero on every call, so ngtcp2
		// never observes a path change and never attempts connection migration. The actual
		// peer address this connection talks to is CQuicNgtcp2Connection's own m_address/m_port,
		// used when flushing output -- not anything ngtcp2 derives from this path.
		return ngtcp2_conn_read_pkt(it->second.conn, &path.path, nullptr, data, length, ts) == 0;
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
		const ngtcp2_tstamp ts = NanosecondsFromMs(nowMs);
		CloseBurstBefore(it->second, ts);
		return FlushConnection(it->second, sink, address, port, ts);
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
		// First, so the expiry below includes the pacing deadline of the burst it closes.
		CloseBurstBefore(it->second, ts);
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
			if (cid.size() == scid.datalen &&
				std::memcmp(cid.data(), scid.data, scid.datalen) == 0) {
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
		ConnectionInfo &info = it->second;
		const ngtcp2_tstamp ts = NanosecondsFromMs(nowMs);
		CloseBurstBefore(info, ts);
		// Packet after packet in the one burst NoteBurstWrite() keeps open, until ngtcp2 takes
		// nothing more (pacing, congestion, flow control) or the data runs out. Bounded like
		// FlushConnection(), so a caller with a large queue cannot hold this thread indefinitely.
		size_t accepted = 0;
		for (int round = 0; round < kMaxFlushPacketsPerCall && accepted < length; ++round) {
			bool packetSent = false;
			const std::ptrdiff_t n = WriteStreamPacket(info,
				data + accepted,
				length - accepted,
				sink,
				address,
				port,
				ts,
				packetSent);
			if (n < 0) {
				return -1;
			}
			// A packet of ACKs or retransmissions with no room left for new data is progress too.
			if (n == 0 && !packetSent) {
				break;
			}
			accepted += static_cast<size_t>(n);
		}
		return static_cast<std::ptrdiff_t>(accepted);
	}

	void ShutdownStream(Handle handle) override
	{
		auto it = m_connections.find(handle);
		if (it != m_connections.end() && it->second.streamId >= 0) {
			ngtcp2_conn_shutdown_stream(it->second.conn, 0, it->second.streamId, 0);
		}
	}

	void CloseStreamGracefully(Handle handle,
		IQuicDatagramSink &sink,
		const CNetworkAddress &address,
		uint16_t port,
		uint64_t nowMs) override
	{
		auto it = m_connections.find(handle);
		if (it == m_connections.end() || it->second.streamId < 0) {
			return;
		}
		const ngtcp2_tstamp ts = NanosecondsFromMs(nowMs);
		CloseBurstBefore(it->second, ts);
		if (!TryWriteGracefulFin(it->second, sink, address, port, ts)) {
			// Pacing/congestion kept this from fitting into a packet right now: ngtcp2 does not
			// record the FIN until it actually serializes, so retry on the next real
			// FlushConnection() call instead of giving up here.
			it->second.pendingGracefulFin = true;
		}
	}

	bool IsStreamEnded(Handle handle) const override
	{
		auto it = m_connections.find(handle);
		return it != m_connections.end() && it->second.streamEnded;
	}

	void EndConnection(Handle handle,
		IQuicDatagramSink &sink,
		const CNetworkAddress &address,
		uint16_t port,
		uint64_t nowMs) override
	{
		auto it = m_connections.find(handle);
		if (it == m_connections.end()) {
			return;
		}
		uint8_t buf[kMaxUdpPayload];
		ngtcp2_path_storage path;
		InitZeroPath(path);
		ngtcp2_pkt_info pi = {};
		ngtcp2_ccerr ccerr;
		ngtcp2_ccerr_default(&ccerr);
		const ngtcp2_tstamp ts = NanosecondsFromMs(nowMs);
		CloseBurstBefore(it->second, ts);
		// A best-effort courtesy, not a requirement for correctness on this side: Destroy() below
		// reclaims the slot either way, whether or not the peer ever sees this.
		const ngtcp2_ssize written = ngtcp2_conn_write_connection_close(
			it->second.conn, &path.path, &pi, buf, sizeof(buf), &ccerr, ts);
		if (written > 0) {
			sink.SendDatagram(buf, static_cast<size_t>(written), address, port);
		}
		Destroy(handle);
	}

	void NotifyWritable(Handle handle) override
	{
		auto it = m_connections.find(handle);
		if (it != m_connections.end() && it->second.transport != nullptr) {
			it->second.transport->OnWritable();
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
		//! ngtcp2_conn_writev_stream() requires the bytes covered by its returned *pdatalen to stay
		//! intact until acked_stream_data_offset() confirms them or the stream closes: it keeps the
		//! raw pointer from the ngtcp2_vec it was given and reads from it again to retransmit, it
		//! does not copy. WriteStreamData()'s caller (CQuicSocketTransport::Flush()) frees its own
		//! copy the moment that call returns, so the ngtcp2_vec must point here instead -- one
		//! std::vector per accepted WriteStreamData() call, so a chunk's address never moves while
		//! unacknowledged (unlike erasing from the front of one shared buffer, which would). Each
		//! is resized down, never up, to whatever ngtcp2 actually consumed, and only ever erased
		//! whole, from the front, once OnAckedStreamData() confirms it in full.
		std::deque<std::vector<uint8_t>> unackedSendChunks;
		size_t unackedSendBytes = 0; //!< Sum of unackedSendChunks sizes; the size cap reads this.
		size_t frontChunkAcked = 0; //!< Bytes of unackedSendChunks.front() acked_stream_data_offset()
					    //!< already confirmed.
		//! Set by OnRecvStreamData()'s FIN flag, OnStreamClose(), or OnStreamReset() -- whichever
		//! notices first that the one stream this connection will ever have is done.
		bool streamEnded = false;
		//! CloseStreamGracefully() asked for a FIN that writev_stream() could not fit into a
		//! packet right then (pacing/congestion: ngtcp2 does not record the FIN internally unless
		//! the frame actually got serialized, so a blocked attempt is not retried on its own).
		//! FlushConnection() clears this once a real FlushConnection() call -- with a later,
		//! genuinely advanced ts -- gets it out.
		bool pendingGracefulFin = false;
		//! The write burst still open: everything written at burstTs since the last
		//! ngtcp2_conn_update_pkt_tx_time(). See NoteBurstWrite().
		bool burstOpen = false;
		ngtcp2_tstamp burstTs = 0;
	};

	//! Closes the open burst once time has moved on. Must run before any ngtcp2 call at @p ts:
	//! update_pkt_tx_time() takes the burst's own timestamp, and ngtcp2 requires timestamps that
	//! never go backwards.
	static void CloseBurstBefore(ConnectionInfo &connection, ngtcp2_tstamp ts)
	{
		if (connection.burstOpen && ts > connection.burstTs) {
			ngtcp2_conn_update_pkt_tx_time(connection.conn, connection.burstTs);
			connection.burstOpen = false;
		}
	}

	//! Every writev_stream() at one timestamp is one burst, which ngtcp2 paces as a whole once
	//! CloseBurstBefore() ends it. Ending it after each packet instead means the next packet at
	//! that timestamp is always paced on ngtcp2 1.13 and later, which leaves this engine one
	//! packet per datagram or tick. Not capped at the send quantum: this engine runs on a datagram
	//! or a tick, never at the pacing deadline, so the cap would allow one quantum per event.
	static void NoteBurstWrite(ConnectionInfo &connection, ngtcp2_tstamp ts)
	{
		connection.burstOpen = true;
		connection.burstTs = ts;
	}

	//! One packet of WriteStreamData(): the stream bytes ngtcp2 took (0 if none fit), or -1 on a
	//! fatal error. @p packetSent: whether a packet went out, with or without stream data.
	std::ptrdiff_t WriteStreamPacket(ConnectionInfo &info,
		const uint8_t *data,
		size_t length,
		IQuicDatagramSink &sink,
		const CNetworkAddress &address,
		uint16_t port,
		ngtcp2_tstamp ts,
		bool &packetSent)
	{
		// A peer that stops ACKing is already a dead connection from this engine's point of view,
		// so refusing to grow past the same window CQuicSocketTransport imposes on the application
		// side (kReadWindow, also 256KiB) is a bound, not a behaviour change for any peer actually
		// speaking QUIC. Offer only what still fits, the same way ngtcp2 itself accepting less than
		// offered is already handled below -- @p length is the caller's whole queued chunk (up to
		// CQuicSocketTransport::kWriteBound, also 256KiB), not what ngtcp2 is actually about to
		// accept (one packet's worth), so treating the full length as what must fit would abort the
		// stream over a single byte still outstanding, not an actual lack of room.
		if (info.unackedSendBytes >= CQuicSocketTransport::kReadWindow) {
			return 0;
		}
		// One STREAM frame never carries more than a packet, so copying more only inflates the
		// retained capacity of unackedSendChunks.
		const size_t offeredLength = std::min({ length,
			CQuicSocketTransport::kReadWindow - info.unackedSendBytes,
			kMaxUdpPayload });
		// Our own copy, created before ngtcp2 ever sees it: the ngtcp2_vec below must point at
		// memory we control for as long as ngtcp2 might still need it (ConnectionInfo::
		// unackedSendChunks's comment has the full contract), not the caller's -- which
		// CQuicSocketTransport::Flush() frees the instant this call returns.
		info.unackedSendChunks.emplace_back(data, data + offeredLength);
		uint8_t *ours = info.unackedSendChunks.back().data();
		info.unackedSendBytes += offeredLength;

		uint8_t buf[kMaxUdpPayload];
		ngtcp2_path_storage path;
		InitZeroPath(path);
		ngtcp2_pkt_info pi = {};
		const ngtcp2_vec vec{ ours, offeredLength };
		ngtcp2_ssize dataLen = 0;
		// ngtcp2_conn_writev_stream() both accepts stream data and may produce a packet in the
		// same call: there is no separate "queue it for later" step to split this into, unlike
		// CUtpSocketTransport's push model over libutp's own internal timer.
		const ngtcp2_ssize written = ngtcp2_conn_writev_stream(info.conn,
			&path.path,
			&pi,
			buf,
			sizeof(buf),
			&dataLen,
			NGTCP2_WRITE_STREAM_FLAG_NONE,
			info.streamId,
			&vec,
			1,
			ts);
		NoteBurstWrite(info, ts);
		std::ptrdiff_t result;
		if (written < 0) {
			// Nothing was sent, so nothing of this chunk is actually pending retransmission.
			info.unackedSendBytes -= offeredLength;
			info.unackedSendChunks.pop_back();
			// STREAM_DATA_BLOCKED is documented as non-fatal: the stream is flow-control blocked,
			// not the connection -- the peer extending the window (or NotifyWritable() noticing
			// congestion eased) is what unblocks it, not tearing anything down. Every other
			// negative return here is a genuine, fatal error.
			result = written == NGTCP2_ERR_STREAM_DATA_BLOCKED ? 0 : -1;
		} else {
			// ngtcp2 can take less than the full chunk (e.g. congestion-limited): shrink our copy
			// down to exactly what it consumed, or drop it if it took nothing. Shrinking a vector
			// never moves its buffer, so this cannot invalidate a pointer ngtcp2 is still holding
			// onto the portion it did accept. The untaken tail is never ngtcp2's concern: the
			// caller (CQuicSocketTransport::Flush(), via its own front-offset tracking) still has
			// it and will offer it again on the next Flush().
			const size_t accepted = dataLen > 0 ? static_cast<size_t>(dataLen) : 0;
			if (accepted < offeredLength) {
				info.unackedSendBytes -= (offeredLength - accepted);
				if (accepted == 0) {
					info.unackedSendChunks.pop_back();
				} else {
					info.unackedSendChunks.back().resize(accepted);
				}
			}
			packetSent = written > 0;
			result = (written > 0 &&
					 !sink.SendDatagram(buf, static_cast<size_t>(written), address, port))
					 ? -1
					 : (dataLen < 0 ? 0 : static_cast<std::ptrdiff_t>(dataLen));
		}
		return result;
	}

	//! Returns true once the FIN frame was actually serialized -- not just that some packet
	//! went out, since ngtcp2_conn_writev_stream() documents that other frames (e.g. an ACK)
	//! can fill the packet instead, leaving *pdatalen at -1 and the FIN unsent despite
	//! `written > 0`. A FIN-only call with no data serializes as *pdatalen == 0 on success.
	bool TryWriteGracefulFin(ConnectionInfo &connection,
		IQuicDatagramSink &sink,
		const CNetworkAddress &address,
		uint16_t port,
		ngtcp2_tstamp ts)
	{
		uint8_t buf[kMaxUdpPayload];
		ngtcp2_path_storage path;
		InitZeroPath(path);
		ngtcp2_pkt_info pi = {};
		ngtcp2_ssize dataLen = 0;
		// No vec at all, not an empty one: an empty ngtcp2_vec would still count as "stream data
		// was offered", which is not what a close with nothing left to send should claim.
		const ngtcp2_ssize written = ngtcp2_conn_writev_stream(connection.conn,
			&path.path,
			&pi,
			buf,
			sizeof(buf),
			&dataLen,
			NGTCP2_WRITE_STREAM_FLAG_FIN,
			connection.streamId,
			nullptr,
			0,
			ts);
		NoteBurstWrite(connection, ts);
		if (written > 0) {
			sink.SendDatagram(buf, static_cast<size_t>(written), address, port);
		}
		return written > 0 && dataLen == 0;
	}

	// Shared by Flush() (an inbound datagram may need an immediate reply) and Tick() (a timer
	// firing with nothing freshly received still needs to retransmit or probe): both end in
	// exactly the same bounded send loop.
	bool FlushConnection(ConnectionInfo &connection,
		IQuicDatagramSink &sink,
		const CNetworkAddress &address,
		uint16_t port,
		ngtcp2_tstamp ts)
	{
		if (connection.pendingGracefulFin) {
			if (TryWriteGracefulFin(connection, sink, address, port, ts)) {
				connection.pendingGracefulFin = false;
			}
			// Whether or not it fit this time, the loop below still services this connection's
			// own retransmission/ACK traffic the same as any other call -- a pending FIN is not a
			// reason to skip that.
		}
		// A bounded number of packets per flush, not an unbounded drain-until-0 loop: a
		// connection that always has more to send (e.g. a peer that never acknowledges) must
		// not be able to make one flush call monopolize this thread indefinitely.
		bool ok = true;
		for (int round = 0; round < kMaxFlushPacketsPerCall; ++round) {
			uint8_t buf[kMaxUdpPayload];
			ngtcp2_path_storage path;
			InitZeroPath(path);
			ngtcp2_pkt_info pi = {};
			// stream_id -1, no data: this call only flushes what ngtcp2 itself needs to send
			// (handshake CRYPTO frames, ACKs, retransmissions, probes). Application data goes
			// out through WriteStreamData() instead, which can itself produce a packet in the
			// same call.
			const ngtcp2_ssize written = ngtcp2_conn_writev_stream(connection.conn,
				&path.path,
				&pi,
				buf,
				sizeof(buf),
				nullptr,
				NGTCP2_WRITE_STREAM_FLAG_NONE,
				-1,
				nullptr,
				0,
				ts);
			NoteBurstWrite(connection, ts);
			if (written < 0) {
				ok = false;
				break;
			}
			if (written == 0) {
				break;
			}
			if (!sink.SendDatagram(buf, static_cast<size_t>(written), address, port)) {
				ok = false;
				break;
			}
		}
		return ok;
	}

	std::map<Handle, ConnectionInfo> m_connections;
};
} // namespace

CQuicNgtcp2Factory::CQuicNgtcp2Factory(const CQuicTlsPolicy &policy,
	std::shared_ptr<IQuicDatagramSink> sink,
	std::shared_ptr<IQuicNgtcp2Engine> engine,
	const std::array<uint8_t, 16> &localIdentity,
	std::function<uint64_t()> transportClock)
: m_policy(policy)
, m_sink(std::move(sink))
, m_localIdentity(localIdentity)
, m_engine(std::move(engine))
, m_transportClock(std::move(transportClock))
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
	const std::string &cid,
	uint64_t nowMs)
{
	CQuicInitialMetadata metadata;
	if (m_policy.credentials == nullptr || m_sink == nullptr || m_engine == nullptr || cid.empty() ||
		cid.size() > 20 || !ParseInitialMetadata(data, length, metadata) ||
		metadata.destinationCid != cid) {
		return nullptr;
	}
	IQuicNgtcp2Engine::Handle handle = m_engine->CreateServer(m_policy, address, port, metadata, nowMs);
	if (handle == nullptr)
		return nullptr;
	std::string issuedCid = m_engine->GetIssuedConnectionId(handle);
	return std::make_unique<CQuicNgtcp2Connection>(m_engine,
		m_sink,
		handle,
		address,
		port,
		issuedCid,
		m_acceptor,
		m_localIdentity,
		m_transportClock);
}

std::shared_ptr<IQuicNgtcp2Engine> CreateProductionQuicNgtcp2Engine()
{
	return std::make_shared<CProductionNgtcp2Engine>();
}
