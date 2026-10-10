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

// A genuine two-endpoint QUIC + TLS 1.3 handshake against the real production engine. Every
// other QUIC test either injects a fake IQuicNgtcp2Engine or exercises the real one from only
// one side (a single Initial, proven to be rejected or admitted). None of them can prove a
// handshake actually completes, because none of them has a QUIC client to complete it with --
// this file is that client, built from raw ngtcp2 + GnuTLS calls, existing only to drive this
// one test.

#include <muleunit/test.h>
#include <NetworkAddress.h>
#include <QuicContext.h>
#include <QuicGnuTlsSession.h>
#include <QuicNattProtocol.h>
#include <QuicNgtcp2Adapter.h>
#include <QuicSocketTransport.h>
#include <QuicStreamAcceptor.h>

#include <gnutls/crypto.h>
#include <gnutls/gnutls.h>
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_gnutls.h>

#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace muleunit;
DECLARE_SIMPLE(QuicNgtcp2Handshake)

namespace
{
const CNetworkAddress kPeer = CNetworkAddress::FromString("192.0.2.1");
//! Arbitrary: the server never checks the sender's half of the proof against anything it
//! already expected (QuicNgtcp2Adapter.cpp's TryExchangeEaqn1Proof() passes a null
//! expectedPeerHash -- there is no prior rendezvous context for an inbound connection).
const std::array<uint8_t, 16> kTestClientIdentity{
	101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116
};
const std::array<uint8_t, 16> kTestServerIdentity{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };

void EnsureGnuTlsInitialized()
{
	static std::once_flag flag;
	std::call_once(flag, [] { gnutls_global_init(); });
}

int AcceptAnyCertificate(gnutls_session_t)
{
	return 0;
}

ngtcp2_conn *GetConnFromRef(ngtcp2_crypto_conn_ref *ref)
{
	return static_cast<ngtcp2_conn *>(ref->user_data);
}

void Ngtcp2Random(uint8_t *dest, size_t destlen, const ngtcp2_rand_ctx *)
{
	gnutls_rnd(GNUTLS_RND_NONCE, dest, destlen);
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

// Mirrors InitZeroPath() in QuicNgtcp2Adapter.cpp: ngtcp2's sockaddr_eq() aborts on an unset
// sa_family, so "no real path" still has to be a real, zero-address AF_INET endpoint.
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

struct ForeignVerifier : IQuicTlsVerifier
{
};

// CQuicNgtcp2Factory::CreateInbound() only checks this for non-nullness before admitting a
// connection -- the real per-connection GnuTLS session CProductionNgtcp2Engine::CreateServer()
// actually uses comes from policy.credentials, not this field. Any non-null value satisfies it.
struct ForeignSession : IQuicNgtcp2TlsSession
{
	uint8_t token = 0;
	gnutls_session_t NativeGnuTlsSession() const override
	{
		return reinterpret_cast<gnutls_session_t>(const_cast<uint8_t *>(&token));
	}
	bool ConfigureTls13Alpn(
		const IQuicTlsCredentials &, const IQuicTlsVerifier *, const uint8_t *, size_t, bool) override
	{
		return true;
	}
};

std::string ExtractInitialDcid(const std::vector<uint8_t> &datagram)
{
	ngtcp2_pkt_hd header;
	if (ngtcp2_accept(&header, datagram.data(), datagram.size()) != 0) {
		return {};
	}
	return std::string(reinterpret_cast<const char *>(header.dcid.data), header.dcid.datalen);
}

struct CollectingSink : IQuicDatagramSink
{
	std::vector<std::vector<uint8_t>> sent;
	bool SendDatagram(const uint8_t *p, size_t n, const CNetworkAddress &, uint16_t) override
	{
		sent.emplace_back(p, p + n);
		return true;
	}
};

// The minimal QUIC client this codebase otherwise has none of: real GnuTLS session and
// credentials, real ngtcp2_conn, driven only far enough to prove a handshake with the real
// server engine completes.
class CTestQuicClient
{
public:
	bool Init(size_t streamWindow = CQuicSocketTransport::kReadWindow)
	{
		EnsureGnuTlsInitialized();
		if (gnutls_certificate_allocate_credentials(&m_credentials) != GNUTLS_E_SUCCESS) {
			return false;
		}
		gnutls_certificate_set_verify_function(m_credentials, AcceptAnyCertificate);
		if (gnutls_init(&m_session, GNUTLS_CLIENT) != GNUTLS_E_SUCCESS) {
			return false;
		}
		if (gnutls_priority_set_direct(m_session, "NORMAL:-VERS-ALL:+VERS-TLS1.3", nullptr) !=
			GNUTLS_E_SUCCESS) {
			return false;
		}
		if (gnutls_credentials_set(m_session, GNUTLS_CRD_CERTIFICATE, m_credentials) !=
			GNUTLS_E_SUCCESS) {
			return false;
		}
		const auto *alpnBytes = reinterpret_cast<const unsigned char *>(QuicNatt::QUIC_NATT_ALPN);
		const gnutls_datum_t alpn{ const_cast<unsigned char *>(alpnBytes),
			static_cast<unsigned>(sizeof(QuicNatt::QUIC_NATT_ALPN) - 1) };
		if (gnutls_alpn_set_protocols(m_session, &alpn, 1, GNUTLS_ALPN_MANDATORY) !=
			GNUTLS_E_SUCCESS) {
			return false;
		}
		if (ngtcp2_crypto_gnutls_configure_client_session(m_session) != 0) {
			return false;
		}

		ngtcp2_cid dcid = {};
		ngtcp2_cid scid = {};
		dcid.datalen = 8;
		scid.datalen = 8;
		Ngtcp2Random(dcid.data, dcid.datalen, nullptr);
		Ngtcp2Random(scid.data, scid.datalen, nullptr);

		ngtcp2_path_storage path;
		InitZeroPath(path);

		ngtcp2_callbacks callbacks = {};
		callbacks.client_initial = ngtcp2_crypto_client_initial_cb;
		callbacks.recv_retry = ngtcp2_crypto_recv_retry_cb;
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
		// ngtcp2 1.9.1 has no getter for handshake confirmation (RFC 9000 section 4.1.2: both
		// endpoints agree the handshake has finished) -- only this callback.
		callbacks.handshake_confirmed = [](ngtcp2_conn *, void *userData) -> int {
			static_cast<CTestQuicClient *>(userData)->m_handshakeConfirmed = true;
			return 0;
		};
		// Lets this test prove the server's real WriteStream()/WriteStreamData() path by reading
		// back what the server actually sent, not just that the client accepted the datagram.
		callbacks.recv_stream_data = [](ngtcp2_conn *,
						     uint32_t,
						     int64_t,
						     uint64_t,
						     const uint8_t *data,
						     size_t datalen,
						     void *userData,
						     void *) -> int {
			auto &received = static_cast<CTestQuicClient *>(userData)->m_received;
			received.insert(received.end(), data, data + datalen);
			return 0;
		};

		ngtcp2_settings settings = {};
		ngtcp2_settings_default(&settings);
		ngtcp2_transport_params params = {};
		ngtcp2_transport_params_default(&params);
		// ngtcp2_transport_params_default() leaves every flow-control window at 0, meaning this
		// client would advertise zero willingness to receive anything back on the one stream it
		// opens -- fine for proving the handshake alone, but WriteStreamData() on the real server
		// correctly refuses to send a single byte against that. Match CProductionNgtcp2Engine's
		// own server-side windows (QuicNgtcp2Adapter.cpp) so the reply path has somewhere to land.
		params.initial_max_stream_data_bidi_local = streamWindow;
		params.initial_max_data = CQuicSocketTransport::kReadWindow;

		if (ngtcp2_conn_client_new(&m_conn,
			    &dcid,
			    &scid,
			    &path.path,
			    NGTCP2_PROTO_VER_V1,
			    &callbacks,
			    &settings,
			    &params,
			    nullptr,
			    this) != 0 ||
			m_conn == nullptr) {
			return false;
		}
		ngtcp2_conn_set_tls_native_handle(m_conn, m_session);
		m_connRef.get_conn = GetConnFromRef;
		m_connRef.user_data = m_conn;
		gnutls_session_set_ptr(m_session, &m_connRef);
		return true;
	}

	~CTestQuicClient()
	{
		if (m_conn != nullptr) {
			ngtcp2_conn_del(m_conn);
		}
		if (m_session != nullptr) {
			gnutls_deinit(m_session);
		}
		if (m_credentials != nullptr) {
			gnutls_certificate_free_credentials(m_credentials);
		}
	}

	//! Everything this client currently has to send, as separate datagrams, in order.
	std::vector<std::vector<uint8_t>> Pump(uint64_t nowMs)
	{
		std::vector<std::vector<uint8_t>> out;
		const ngtcp2_tstamp ts = nowMs * UINT64_C(1000000);
		for (int round = 0; round < kMaxPacketsPerPump; ++round) {
			uint8_t buf[kMaxUdpPayload];
			ngtcp2_path_storage path;
			InitZeroPath(path);
			ngtcp2_pkt_info pi = {};
			const ngtcp2_ssize written = ngtcp2_conn_writev_stream(
				m_conn, &path.path, &pi, buf, sizeof(buf), nullptr, 0, -1, nullptr, 0, ts);
			if (written <= 0) {
				break;
			}
			out.emplace_back(buf, buf + written);
		}
		return out;
	}

	bool Receive(const std::vector<uint8_t> &datagram, uint64_t nowMs)
	{
		ngtcp2_path_storage path;
		InitZeroPath(path);
		const ngtcp2_tstamp ts = nowMs * UINT64_C(1000000);
		return ngtcp2_conn_read_pkt(
			       m_conn, &path.path, nullptr, datagram.data(), datagram.size(), ts) == 0;
	}

	//! Opens this client's one bidirectional stream on first use, then sends @p payload on it,
	//! as separate outgoing datagrams. eD2k only ever replies on the peer's stream
	//! (QuicSocketTransport.h), so the server side never opens one of its own. @p fin marks this
	//! as the last data this client will ever send on the stream (NGTCP2_STREAM_DATA_FLAG_FIN on
	//! the server's receiving end).
	std::vector<std::vector<uint8_t>> SendOnStream(
		const std::vector<uint8_t> &payload, uint64_t nowMs, bool fin = false)
	{
		if (m_streamId < 0 && ngtcp2_conn_open_bidi_stream(m_conn, &m_streamId, nullptr) != 0) {
			return {};
		}
		std::vector<std::vector<uint8_t>> out;
		const ngtcp2_tstamp ts = nowMs * UINT64_C(1000000);
		const ngtcp2_vec vec{ const_cast<uint8_t *>(payload.data()), payload.size() };
		const uint32_t flags = fin ? NGTCP2_WRITE_STREAM_FLAG_FIN : NGTCP2_WRITE_STREAM_FLAG_NONE;
		bool dataSent = payload.empty();
		for (int round = 0; round < kMaxPacketsPerPump; ++round) {
			uint8_t buf[kMaxUdpPayload];
			ngtcp2_path_storage path;
			InitZeroPath(path);
			ngtcp2_pkt_info pi = {};
			ngtcp2_ssize dataLen = 0;
			const ngtcp2_ssize written = ngtcp2_conn_writev_stream(m_conn,
				&path.path,
				&pi,
				buf,
				sizeof(buf),
				&dataLen,
				flags,
				m_streamId,
				dataSent ? nullptr : &vec,
				dataSent ? 0 : 1,
				ts);
			if (written <= 0) {
				break;
			}
			if (dataLen > 0) {
				dataSent = true;
			}
			out.emplace_back(buf, buf + written);
		}
		return out;
	}

	bool HandshakeConfirmed() const { return m_handshakeConfirmed; }
	const std::vector<uint8_t> &Received() const { return m_received; }
	void ClearReceived() { m_received.clear(); }
	//! The real-world equivalent of an application finishing a Read() and reopening that much
	//! credit -- CQuicSocketTransport::ExtendReadWindow()'s counterpart on this side, needed to
	//! recover from a stream this client itself blocked by advertising a small window.
	void ExtendReadWindow(size_t bytes)
	{
		ngtcp2_conn_extend_max_stream_offset(m_conn, m_streamId, bytes);
		ngtcp2_conn_extend_max_offset(m_conn, bytes);
	}

private:
	static constexpr size_t kMaxUdpPayload = 1452;
	static constexpr int kMaxPacketsPerPump = 16;

	gnutls_certificate_credentials_t m_credentials = nullptr;
	gnutls_session_t m_session = nullptr;
	ngtcp2_conn *m_conn = nullptr;
	ngtcp2_crypto_conn_ref m_connRef{};
	bool m_handshakeConfirmed = false;
	int64_t m_streamId = -1;
	std::vector<uint8_t> m_received;
};

// Shared by every test here: drives Initial exchange through handshake confirmation against the
// real production engine. Bounded by kMaxRounds so a genuine protocol regression fails fast and
// deterministically instead of hanging.
bool DriveHandshake(CTestQuicClient &client,
	CQuicNgtcp2Factory &factory,
	CollectingSink &sink,
	std::unique_ptr<IQuicConnection> &connection,
	uint64_t &nowMs)
{
	bool confirmed = false;
	constexpr int kMaxRounds = 20;
	for (int round = 0; round < kMaxRounds && !confirmed; ++round, nowMs += 10) {
		for (const auto &datagram : client.Pump(nowMs)) {
			if (!connection) {
				const std::string dcid = ExtractInitialDcid(datagram);
				if (dcid.empty()) {
					return false;
				}
				connection = factory.CreateInbound(
					datagram.data(), datagram.size(), kPeer, 4672, dcid, nowMs);
				if (!connection) {
					return false;
				}
			}
			if (!connection->ProcessDatagram(datagram.data(), datagram.size(), nowMs)) {
				return false;
			}
		}
		for (const auto &datagram : sink.sent) {
			if (!client.Receive(datagram, nowMs)) {
				return false;
			}
		}
		sink.sent.clear();
		confirmed = client.HandshakeConfirmed();
	}
	return confirmed;
}

//! eMuleAI's EAQN1 hand-off (QuicNattProtocol.h, NgTcp2GnuTlsBridge.cpp:711-828): the client's
//! proof must be the first bytes on the stream, before any application data, and the server's
//! own proof must come back before either side treats the stream as ready. Every real-engine
//! test needs this immediately after DriveHandshake() and before sending anything else -- a
//! stream that opens without it never gets past CQuicNgtcp2Connection::TryExchangeEaqn1Proof().
bool ExchangeEaqn1Proof(CTestQuicClient &client,
	std::unique_ptr<IQuicConnection> &connection,
	CollectingSink &sink,
	uint64_t &nowMs)
{
	// Target left zero (nullptr): this is the client's first contact, so it does not yet know
	// which server identity it is confirming -- ValidateEaqn1Proof() accepts that explicitly.
	const auto clientProof = QuicNatt::BuildEaqn1Proof(kTestClientIdentity, nullptr);
	for (const auto &datagram :
		client.SendOnStream(std::vector<uint8_t>(clientProof.begin(), clientProof.end()), nowMs)) {
		if (!connection->ProcessDatagram(datagram.data(), datagram.size(), nowMs)) {
			return false;
		}
	}
	for (const auto &datagram : sink.sent) {
		if (!client.Receive(datagram, nowMs)) {
			return false;
		}
	}
	sink.sent.clear();
	if (client.Received().size() < QuicNatt::EAQN1_PROOF_SIZE) {
		return false;
	}
	// The server's own proof, already validated by construction (CProductionNgtcp2Engine really
	// sent it, or TryExchangeEaqn1Proof() would have closed the connection instead) -- cleared so
	// the tests that follow compare Received() against application data alone.
	client.ClearReceived();
	return true;
}

//! Admits every offered stream and keeps the transport, the same shape CQuicStreamAcceptor
//! gives a caller on success -- without theApp, which the real acceptor's admission policy
//! needs and no unit test here constructs.
struct AcceptingAcceptor : IQuicStreamAcceptor
{
	std::unique_ptr<IStreamTransport> accepted;
	CNetworkAddress lastAddress;
	uint16_t lastPort = 0;

	bool AcceptStream(std::unique_ptr<IStreamTransport> &transport,
		const CNetworkAddress &address,
		uint16_t port) override
	{
		accepted = std::move(transport);
		lastAddress = address;
		lastPort = port;
		return true;
	}
};
} // namespace

TEST(QuicNgtcp2Handshake, ARealClientAndTheRealServerEngineCompleteTheHandshake)
{
	CQuicEphemeralCredentials credentials;
	ASSERT_TRUE(credentials.NativeGnuTlsCredentials() != nullptr);
	ForeignSession session;
	ForeignVerifier verifier;
	CQuicTlsPolicy policy{ &credentials };

	auto sink = std::make_shared<CollectingSink>();
	auto engine = CreateProductionQuicNgtcp2Engine();
	CQuicNgtcp2Factory factory(policy, sink, engine, kTestServerIdentity);

	CTestQuicClient client;
	ASSERT_TRUE(client.Init());

	// Not CQuicContext: it owns connections privately and exposes no way to reach one again,
	// which this test needs to drain the stream data it is about to prove arrives. There is
	// only one connection here, so the routing CQuicContext would otherwise do is unnecessary --
	// every datagram after the first just goes straight to it.
	std::unique_ptr<IQuicConnection> connection;
	uint64_t nowMs = 0;
	ASSERT_TRUE(DriveHandshake(client, factory, *sink, connection, nowMs));
	ASSERT_TRUE(connection != nullptr);

	// Proves the stream path end to end, not just the handshake: real payload, sent on the
	// client's real stream, delivered through the real engine's recv_stream_data callback.
	const std::vector<uint8_t> payload{ 'h', 'i' };
	for (const auto &datagram : client.SendOnStream(payload, nowMs)) {
		ASSERT_TRUE(connection->ProcessDatagram(datagram.data(), datagram.size(), nowMs));
	}
	for (const auto &datagram : sink->sent) {
		ASSERT_TRUE(client.Receive(datagram, nowMs));
	}
	sink->sent.clear();

	const std::vector<uint8_t> received = connection->DrainStreamData();
	ASSERT_TRUE(received == payload);

	// Proves ExtendStreamReadWindow() reaches ngtcp2_conn_extend_max_stream_offset()/
	// extend_max_offset() without crashing once a real stream exists -- the actual point of
	// calling it, as opposed to the no-op default every IQuicConnection has before one does.
	connection->ExtendStreamReadWindow(received.size());

	// Proves the reply path too: CQuicNgtcp2Connection implements IQuicStreamOperations
	// directly (QuicNgtcp2Adapter.cpp), the same object as the IQuicConnection above. A real
	// CClientTCPSocket-attached CQuicSocketTransport would reach WriteStream() through that
	// interface; this test reaches it the same way, without needing a transport at all.
	auto *streamOps = dynamic_cast<IQuicStreamOperations *>(connection.get());
	ASSERT_TRUE(streamOps != nullptr);
	const std::vector<uint8_t> reply{ 'o', 'k' };
	const std::ptrdiff_t written = streamOps->WriteStream(nullptr, reply.data(), reply.size(), nowMs);
	ASSERT_EQUALS(static_cast<std::ptrdiff_t>(reply.size()), written);
	ASSERT_TRUE(!sink->sent.empty());

	for (const auto &datagram : sink->sent) {
		ASSERT_TRUE(client.Receive(datagram, nowMs));
	}
	sink->sent.clear();

	ASSERT_TRUE(client.Received() == reply);
}

TEST(QuicNgtcp2Handshake, AcceptedStreamHandsOffToARealTransportThatReadsAndWritesThroughIt)
{
	// Proves the hand-off CQuicNgtcp2Connection::OfferStreamIfJustOpened() performs once the
	// peer's stream opens: a real CQuicSocketTransport constructed, offered to the acceptor, and
	// -- once accepted -- every further stream byte delivered to it (AttachTransport()) instead
	// of DrainStreamData()'s buffer, with writes on the transport reaching the real client the
	// same way the direct-engine test above proved WriteStream() does.
	CQuicEphemeralCredentials credentials;
	ForeignSession session;
	ForeignVerifier verifier;
	CQuicTlsPolicy policy{ &credentials };

	// Shared with the transport this factory hands off: its Flush()/Close() must read the same
	// synthetic clock as the ProcessDatagram()/Tick() calls below, not the real one
	// (QuicSocketTransport.h's own clock injection).
	uint64_t nowMs = 0;
	auto sink = std::make_shared<CollectingSink>();
	auto engine = CreateProductionQuicNgtcp2Engine();
	CQuicNgtcp2Factory factory(policy, sink, engine, kTestServerIdentity, [&nowMs] { return nowMs; });
	AcceptingAcceptor acceptor;
	factory.SetAcceptor(&acceptor);

	CTestQuicClient client;
	ASSERT_TRUE(client.Init());

	std::unique_ptr<IQuicConnection> connection;
	ASSERT_TRUE(DriveHandshake(client, factory, *sink, connection, nowMs));
	ASSERT_TRUE(connection != nullptr);
	// No stream has opened yet at this point -- only the handshake has completed -- so the
	// hand-off cannot have happened before the client sends anything.
	ASSERT_TRUE(acceptor.accepted == nullptr);

	// The exchange itself completes the hand-off: CQuicNgtcp2Connection::OfferStreamIfJustOpened()
	// offers the transport to the acceptor in the same call that TryExchangeEaqn1Proof() succeeds
	// in, since the proof is already everything the connection needed to consider this peer worth
	// handing off.
	ASSERT_TRUE(ExchangeEaqn1Proof(client, connection, *sink, nowMs));
	ASSERT_TRUE(acceptor.accepted != nullptr);
	ASSERT_TRUE(acceptor.lastAddress == kPeer);
	ASSERT_EQUALS(4672u, acceptor.lastPort);
	ASSERT_TRUE(acceptor.accepted->IsConnected());

	const std::vector<uint8_t> payload{ 'h', 'i' };
	for (const auto &datagram : client.SendOnStream(payload, nowMs)) {
		ASSERT_TRUE(connection->ProcessDatagram(datagram.data(), datagram.size(), nowMs));
	}
	for (const auto &datagram : sink->sent) {
		ASSERT_TRUE(client.Receive(datagram, nowMs));
	}
	sink->sent.clear();

	uint8_t buf[64] = {};
	const uint32_t readBytes = acceptor.accepted->Read(buf, sizeof(buf));
	ASSERT_EQUALS(static_cast<uint32_t>(payload.size()), readBytes);
	ASSERT_TRUE(std::vector<uint8_t>(buf, buf + readBytes) == payload);

	// DrainStreamData() must now be empty: the bytes above went straight to the transport
	// (AttachTransport()'s whole point), never into the connection's own buffer.
	ASSERT_TRUE(connection->DrainStreamData().empty());

	const std::vector<uint8_t> reply{ 'o', 'k' };
	const uint32_t writtenBytes =
		acceptor.accepted->Write(reply.data(), static_cast<uint32_t>(reply.size()));
	ASSERT_EQUALS(static_cast<uint32_t>(reply.size()), writtenBytes);
	acceptor.accepted->Flush();
	ASSERT_TRUE(!sink->sent.empty());

	for (const auto &datagram : sink->sent) {
		ASSERT_TRUE(client.Receive(datagram, nowMs));
	}
	sink->sent.clear();

	ASSERT_TRUE(client.Received() == reply);
}

TEST(QuicNgtcp2Handshake, LostFirstAttemptIsRetransmittedWithoutCorruptingTheReply)
{
	// ngtcp2_conn_writev_stream() documents that the bytes covered by its *pdatalen must stay
	// intact until acked_stream_data_offset() confirms them: a lost packet makes ngtcp2 read the
	// same bytes again to retransmit. This drops the server's first reply datagram on the floor
	// instead of delivering it -- the one case CollectingSink's always-delivered loopback never
	// exercises elsewhere in this file -- and proves the retransmission still carries the right
	// bytes (and, under ASan, that nothing reads freed memory to produce it).
	CQuicEphemeralCredentials credentials;
	ForeignSession session;
	ForeignVerifier verifier;
	CQuicTlsPolicy policy{ &credentials };

	uint64_t nowMs = 0; // see the clock comment in the previous test
	auto sink = std::make_shared<CollectingSink>();
	auto engine = CreateProductionQuicNgtcp2Engine();
	CQuicNgtcp2Factory factory(policy, sink, engine, kTestServerIdentity, [&nowMs] { return nowMs; });
	AcceptingAcceptor acceptor;
	factory.SetAcceptor(&acceptor);

	CTestQuicClient client;
	ASSERT_TRUE(client.Init());

	std::unique_ptr<IQuicConnection> connection;
	ASSERT_TRUE(DriveHandshake(client, factory, *sink, connection, nowMs));
	ASSERT_TRUE(connection != nullptr);
	ASSERT_TRUE(ExchangeEaqn1Proof(client, connection, *sink, nowMs));
	ASSERT_TRUE(acceptor.accepted != nullptr);

	// Through the real transport, not a direct IQuicStreamOperations call: CQuicSocketTransport's
	// Write()+Flush() is what frees its own copy of the data the instant WriteStream() returns
	// (QuicSocketTransport.cpp's Flush(), pop_front() right after the call) -- a direct call with
	// a test-local std::vector would keep that buffer alive for the rest of this function
	// regardless of what the engine does, proving nothing about the engine's own retention.
	const std::vector<uint8_t> payload{ 'h', 'i' };
	for (const auto &datagram : client.SendOnStream(payload, nowMs)) {
		ASSERT_TRUE(connection->ProcessDatagram(datagram.data(), datagram.size(), nowMs));
	}
	for (const auto &datagram : sink->sent) {
		ASSERT_TRUE(client.Receive(datagram, nowMs));
	}
	sink->sent.clear();

	const std::vector<uint8_t> reply2{ 'o', 'k' };
	const uint32_t writtenBytes =
		acceptor.accepted->Write(reply2.data(), static_cast<uint32_t>(reply2.size()));
	ASSERT_EQUALS(static_cast<uint32_t>(reply2.size()), writtenBytes);
	acceptor.accepted->Flush();
	ASSERT_TRUE(!sink->sent.empty());

	// Dropped, not delivered: this is the packet loss. The transport's own copy is already gone
	// (Flush() just freed it); the only copy left anywhere is whatever WriteStreamData() retained
	// internally.
	sink->sent.clear();

	// Ticks until ngtcp2's own loss-detection timer retransmits, delivering every datagram that
	// produces as it goes: the first one after a loss is not necessarily the retransmission
	// itself (a PTO probe can go out first, carrying no STREAM frame at all) -- only once the
	// client has actually seen reply2's bytes does this stop. Fails loudly instead of hanging if
	// a real protocol regression means that never happens.
	for (int i = 0; i < 200 && client.Received() != reply2; ++i) {
		nowMs += 25;
		connection->Tick(nowMs);
		for (const auto &datagram : sink->sent) {
			ASSERT_TRUE(client.Receive(datagram, nowMs));
		}
		sink->sent.clear();
	}

	ASSERT_TRUE(client.Received() == reply2);
}

// got3nks' review on #1710 (finding #4, High): with exactly one stream per connection by design,
// nothing previously noticed when that stream ended -- the connection just sat there, occupying
// one of CQuicContext::kMaxConnectionsPerScope's slots, until the idle timeout eventually caught
// up with it. Proves the real engine's FIN handling actually ends the connection promptly.
TEST(QuicNgtcp2Handshake, TheStreamEndingClosesTheWholeConnection)
{
	CQuicEphemeralCredentials credentials;
	ForeignSession session;
	ForeignVerifier verifier;
	CQuicTlsPolicy policy{ &credentials };

	auto sink = std::make_shared<CollectingSink>();
	auto engine = CreateProductionQuicNgtcp2Engine();
	CQuicNgtcp2Factory factory(policy, sink, engine, kTestServerIdentity);
	AcceptingAcceptor acceptor;
	factory.SetAcceptor(&acceptor);

	CTestQuicClient client;
	ASSERT_TRUE(client.Init());

	std::unique_ptr<IQuicConnection> connection;
	uint64_t nowMs = 0;
	ASSERT_TRUE(DriveHandshake(client, factory, *sink, connection, nowMs));
	ASSERT_TRUE(ExchangeEaqn1Proof(client, connection, *sink, nowMs));
	ASSERT_TRUE(acceptor.accepted != nullptr);
	ASSERT_TRUE(!connection->IsClosed());

	// fin=true: the client declares it will never send anything else on this stream, the same
	// as eD2k closing its half of a TCP connection would.
	const std::vector<uint8_t> payload{ 'b', 'y', 'e' };
	for (const auto &datagram : client.SendOnStream(payload, nowMs, /*fin=*/true)) {
		connection->ProcessDatagram(datagram.data(), datagram.size(), nowMs);
	}

	ASSERT_TRUE(connection->IsClosed());
}

// got3nks' review on #1710 (finding #5, High): NGTCP2_ERR_STREAM_DATA_BLOCKED is documented as
// non-fatal (the stream is flow-control blocked, not the connection), but previously mapped to
// -1 like any other error, tearing the stream down over something a peer extending its window a
// moment later would otherwise have resolved on its own. This also proves CQuicSocketTransport's
// write path does not busy-loop while blocked, and that IQuicNgtcp2Engine::NotifyWritable() is
// what resumes it once the peer's extended window makes room again.
TEST(QuicNgtcp2Handshake, FlowControlBlockDoesNotLoseTheStreamAndRecoversOnceUnblocked)
{
	CQuicEphemeralCredentials credentials;
	ForeignSession session;
	ForeignVerifier verifier;
	CQuicTlsPolicy policy{ &credentials };

	uint64_t nowMs = 0; // see the clock comment in AcceptedStreamHandsOffToARealTransport...
	auto sink = std::make_shared<CollectingSink>();
	auto engine = CreateProductionQuicNgtcp2Engine();
	CQuicNgtcp2Factory factory(policy, sink, engine, kTestServerIdentity, [&nowMs] { return nowMs; });
	AcceptingAcceptor acceptor;
	factory.SetAcceptor(&acceptor);

	// A small window, deliberately: real peers do not have to advertise
	// CQuicSocketTransport::kReadWindow, and this is what makes the block happen "after a few
	// bytes" instead of needing to push 256KiB of data to prove the same thing.
	constexpr size_t kSmallWindow = 100;
	CTestQuicClient client;
	ASSERT_TRUE(client.Init(kSmallWindow));

	std::unique_ptr<IQuicConnection> connection;
	ASSERT_TRUE(DriveHandshake(client, factory, *sink, connection, nowMs));
	ASSERT_TRUE(ExchangeEaqn1Proof(client, connection, *sink, nowMs));
	ASSERT_TRUE(acceptor.accepted != nullptr);

	// kSmallWindow minus the 37-byte EAQN1 proof the server already sent on this same stream
	// (ExchangeEaqn1Proof) leaves 63 bytes of window before the client has read any of this.
	const std::vector<uint8_t> payload(200, 'A');
	const uint32_t writtenBytes =
		acceptor.accepted->Write(payload.data(), static_cast<uint32_t>(payload.size()));
	ASSERT_EQUALS(static_cast<uint32_t>(payload.size()), writtenBytes);
	acceptor.accepted->Flush();
	ASSERT_TRUE(!sink->sent.empty());
	for (const auto &datagram : sink->sent) {
		ASSERT_TRUE(client.Receive(datagram, nowMs));
	}
	sink->sent.clear();
	// Only part of it fit in the window: proves the first Flush() call did make some progress,
	// the premise the rest of this test depends on.
	ASSERT_TRUE(client.Received().size() < payload.size());
	ASSERT_TRUE(!client.Received().empty());
	const size_t firstChunkSize = client.Received().size();

	// Now fully blocked: nothing left of the window to write into. Flush()'s own fix (only
	// re-request a flush when something actually went out) means nothing happens automatically
	// from here -- a real event loop would wait for OnWritable(), which only NotifyWritable()
	// (via a later Tick()/ProcessDatagram()) ever calls.
	acceptor.accepted->Flush();
	ASSERT_TRUE(sink->sent.empty());
	// The stream (and the transport built on it) must still be alive: STREAM_DATA_BLOCKED is not
	// a reason to lose it.
	ASSERT_TRUE(!connection->IsClosed());
	ASSERT_TRUE(acceptor.accepted->IsOk());

	// The real-world unblock: the peer reads what it has, extends its window, and that reaches
	// the server as an ordinary datagram (MAX_STREAM_DATA/MAX_DATA frames, carried on whatever
	// Pump() next produces -- no stream data of its own needed).
	client.ExtendReadWindow(firstChunkSize);
	bool progressed = false;
	for (int i = 0; i < 50 && !progressed; ++i) {
		nowMs += 10;
		for (const auto &datagram : client.Pump(nowMs)) {
			ASSERT_TRUE(connection->ProcessDatagram(datagram.data(), datagram.size(), nowMs));
		}
		for (const auto &datagram : sink->sent) {
			ASSERT_TRUE(client.Receive(datagram, nowMs));
		}
		sink->sent.clear();
		progressed = client.Received().size() > firstChunkSize;
	}
	ASSERT_TRUE(progressed);
}

// got3nks' review on #1710 (finding #6, Medium): Close() used to ask the engine to reset the
// stream outright (ngtcp2_conn_shutdown_stream()), discarding whatever was still queued in the
// transport's own buffer and never handed to WriteStream() at all -- a clean eD2k close could
// silently drop its last packet. Proves Close() gives that data one last chance to go out first.
TEST(QuicNgtcp2Handshake, CloseFlushesQueuedDataBeforeEndingTheStreamCleanly)
{
	CQuicEphemeralCredentials credentials;
	ForeignSession session;
	ForeignVerifier verifier;
	CQuicTlsPolicy policy{ &credentials };

	uint64_t nowMs = 0; // see the clock comment in AcceptedStreamHandsOffToARealTransport...
	auto sink = std::make_shared<CollectingSink>();
	auto engine = CreateProductionQuicNgtcp2Engine();
	CQuicNgtcp2Factory factory(policy, sink, engine, kTestServerIdentity, [&nowMs] { return nowMs; });
	AcceptingAcceptor acceptor;
	factory.SetAcceptor(&acceptor);

	CTestQuicClient client;
	ASSERT_TRUE(client.Init());

	std::unique_ptr<IQuicConnection> connection;
	ASSERT_TRUE(DriveHandshake(client, factory, *sink, connection, nowMs));
	ASSERT_TRUE(ExchangeEaqn1Proof(client, connection, *sink, nowMs));
	ASSERT_TRUE(acceptor.accepted != nullptr);

	// Write(), then Close() immediately -- no Flush() in between, the exact gap that used to
	// lose this data: Close() itself must be the one giving it a chance to go out.
	const std::vector<uint8_t> payload{ 'd', 'o', 'n', 'e' };
	const uint32_t writtenBytes =
		acceptor.accepted->Write(payload.data(), static_cast<uint32_t>(payload.size()));
	ASSERT_EQUALS(static_cast<uint32_t>(payload.size()), writtenBytes);
	acceptor.accepted->Close();
	ASSERT_TRUE(!sink->sent.empty());

	for (const auto &datagram : sink->sent) {
		ASSERT_TRUE(client.Receive(datagram, nowMs));
	}
	sink->sent.clear();

	ASSERT_TRUE(client.Received() == payload);
}

// Post-merge review audit (independent of got3nks' 9 findings, same PR #1710): the
// unackedSendBytes + length > kReadWindow check in WriteStreamData() compared the caller's
// whole offered chunk against the remaining budget, not what ngtcp2 could actually accept (one
// packet's worth) -- a single prior write still unacknowledged could abort the stream over a
// second write ngtcp2 would have happily split across many packets instead. Proves a large
// write survives when something small is already unacknowledged, rather than ending the stream.
TEST(QuicNgtcp2Handshake, LargeWriteWithUnackedDataPendingIsOfferedPartiallyInsteadOfAbortingTheStream)
{
	CQuicEphemeralCredentials credentials;
	ForeignSession session;
	ForeignVerifier verifier;
	CQuicTlsPolicy policy{ &credentials };

	uint64_t nowMs = 0; // see the clock comment in AcceptedStreamHandsOffToARealTransport...
	auto sink = std::make_shared<CollectingSink>();
	auto engine = CreateProductionQuicNgtcp2Engine();
	CQuicNgtcp2Factory factory(policy, sink, engine, kTestServerIdentity, [&nowMs] { return nowMs; });
	AcceptingAcceptor acceptor;
	factory.SetAcceptor(&acceptor);

	CTestQuicClient client;
	ASSERT_TRUE(client.Init());

	std::unique_ptr<IQuicConnection> connection;
	ASSERT_TRUE(DriveHandshake(client, factory, *sink, connection, nowMs));
	ASSERT_TRUE(ExchangeEaqn1Proof(client, connection, *sink, nowMs));
	ASSERT_TRUE(acceptor.accepted != nullptr);

	// A small first write, never acknowledged, leaves unackedSendBytes non-zero -- the
	// precondition the bug needed.
	const std::vector<uint8_t> first(100, 'a');
	ASSERT_EQUALS(static_cast<uint32_t>(first.size()),
		acceptor.accepted->Write(first.data(), static_cast<uint32_t>(first.size())));
	acceptor.accepted->Flush();
	ASSERT_TRUE(!sink->sent.empty());
	sink->sent.clear(); // Dropped: never delivered to the client, so it stays unacknowledged.

	// The largest single write the transport itself ever allows (CQuicSocketTransport::
	// kReadWindow is also its kWriteBound). With unackedSendBytes already non-zero, the old
	// check would abort here even though ngtcp2 only needed to accept one packet's worth of it.
	std::vector<uint8_t> big(CQuicSocketTransport::kReadWindow, 'b');
	ASSERT_EQUALS(static_cast<uint32_t>(big.size()),
		acceptor.accepted->Write(big.data(), static_cast<uint32_t>(big.size())));
	acceptor.accepted->Flush();

	ASSERT_TRUE(acceptor.accepted->IsConnected());
	ASSERT_TRUE(acceptor.accepted->IsOk());
	ASSERT_TRUE(!sink->sent.empty());
}

// Same audit as above: CQuicSocketTransport::Close() called WriteStream() through Flush() at
// most once, handing ngtcp2 a single packet's worth regardless of how much was queued (up to
// kWriteBound, 256KiB) -- CloseFlushesQueuedDataBeforeEndingTheStreamCleanly above never caught
// this because its payload fit in one packet. Proves a payload spanning many packets still
// arrives in full from one Write() immediately followed by Close(), with no Flush() between.
TEST(QuicNgtcp2Handshake, CloseDrainsAQueueSpanningManyPacketsBeforeEndingTheStreamCleanly)
{
	CQuicEphemeralCredentials credentials;
	ForeignSession session;
	ForeignVerifier verifier;
	CQuicTlsPolicy policy{ &credentials };

	uint64_t nowMs = 0; // see the clock comment in AcceptedStreamHandsOffToARealTransport...
	auto sink = std::make_shared<CollectingSink>();
	auto engine = CreateProductionQuicNgtcp2Engine();
	CQuicNgtcp2Factory factory(policy, sink, engine, kTestServerIdentity, [&nowMs] { return nowMs; });
	AcceptingAcceptor acceptor;
	factory.SetAcceptor(&acceptor);

	CTestQuicClient client;
	ASSERT_TRUE(client.Init());

	std::unique_ptr<IQuicConnection> connection;
	ASSERT_TRUE(DriveHandshake(client, factory, *sink, connection, nowMs));
	ASSERT_TRUE(ExchangeEaqn1Proof(client, connection, *sink, nowMs));
	ASSERT_TRUE(acceptor.accepted != nullptr);

	std::vector<uint8_t> payload(50 * 1024);
	for (size_t i = 0; i < payload.size(); ++i) {
		payload[i] = static_cast<uint8_t>(i);
	}
	ASSERT_EQUALS(static_cast<uint32_t>(payload.size()),
		acceptor.accepted->Write(payload.data(), static_cast<uint32_t>(payload.size())));
	acceptor.accepted->Close();
	ASSERT_TRUE(!sink->sent.empty());

	// kMaxUdpPayload-sized packets exceed ngtcp2's initial congestion window well before this
	// payload is fully out: what Close() could not get through its own bounded loop keeps
	// draining asynchronously afterward (CQuicSocketTransport::m_draining), driven by
	// NotifyWritable() once real ACKs -- delivered back to the server below -- free up room.
	// Round-trips, not a one-shot delivery, are what exercise that path.
	constexpr int kMaxDrainRounds = 50;
	for (int round = 0; round < kMaxDrainRounds && client.Received().size() < payload.size();
		++round, nowMs += 20) {
		for (const auto &datagram : sink->sent) {
			ASSERT_TRUE(client.Receive(datagram, nowMs));
		}
		sink->sent.clear();
		for (const auto &datagram : client.Pump(nowMs)) {
			ASSERT_TRUE(connection->ProcessDatagram(datagram.data(), datagram.size(), nowMs));
		}
		connection->Tick(nowMs);
	}

	ASSERT_EQUALS(payload.size(), client.Received().size());
	ASSERT_TRUE(client.Received() == payload);
}

TEST(QuicNgtcp2Handshake, FlushesAtOneTimestampSendABurstNotOnePacket)
{
	// ngtcp2 1.13 and later always pace a packet written at the same timestamp after
	// ngtcp2_conn_update_pkt_tx_time(). The engine called it after every packet, so it got one
	// packet out per datagram or tick: about a hundredth of the throughput older ngtcp2 gave.
	CQuicEphemeralCredentials credentials;
	CQuicTlsPolicy policy{ &credentials };
	uint64_t nowMs = 0;
	auto sink = std::make_shared<CollectingSink>();
	auto engine = CreateProductionQuicNgtcp2Engine();
	CQuicNgtcp2Factory factory(policy, sink, engine, kTestServerIdentity, [&nowMs] { return nowMs; });
	AcceptingAcceptor acceptor;
	factory.SetAcceptor(&acceptor);

	CTestQuicClient client;
	ASSERT_TRUE(client.Init());
	std::unique_ptr<IQuicConnection> connection;
	ASSERT_TRUE(DriveHandshake(client, factory, *sink, connection, nowMs));
	ASSERT_TRUE(ExchangeEaqn1Proof(client, connection, *sink, nowMs));
	ASSERT_TRUE(acceptor.accepted != nullptr);

	const std::vector<uint8_t> data(32 * 1024, 0x5a);
	ASSERT_EQUALS(static_cast<uint32_t>(data.size()),
		acceptor.accepted->Write(data.data(), static_cast<uint32_t>(data.size())));
	// What the socket's flush requests do within one event: flush again while that gets
	// something out. nowMs does not move.
	for (int round = 0; round < 64; ++round) {
		const size_t before = sink->sent.size();
		acceptor.accepted->Flush();
		if (sink->sent.size() == before) {
			break;
		}
	}
	// The initial congestion window alone admits about ten full packets.
	ASSERT_TRUE(sink->sent.size() >= 5);
}
