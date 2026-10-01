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

#include <gnutls/crypto.h>
#include <gnutls/gnutls.h>
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_gnutls.h>

#include <netinet/in.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace muleunit;
DECLARE_SIMPLE(QuicNgtcp2Handshake)

namespace
{
const CNetworkAddress kPeer = CNetworkAddress::FromString("192.0.2.1");

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
	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	ngtcp2_path_storage_init(&path, reinterpret_cast<ngtcp2_sockaddr *>(&addr), sizeof(addr),
		reinterpret_cast<ngtcp2_sockaddr *>(&addr), sizeof(addr), nullptr);
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
	bool Init()
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
		if (gnutls_alpn_set_protocols(m_session, &alpn, 1, GNUTLS_ALPN_MANDATORY) != GNUTLS_E_SUCCESS) {
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

		ngtcp2_settings settings = {};
		ngtcp2_settings_default(&settings);
		ngtcp2_transport_params params = {};
		ngtcp2_transport_params_default(&params);

		if (ngtcp2_conn_client_new(&m_conn, &dcid, &scid, &path.path, NGTCP2_PROTO_VER_V1, &callbacks,
			    &settings, &params, nullptr, this) != 0 ||
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
	//! (QuicSocketTransport.h), so the server side never opens one of its own.
	std::vector<std::vector<uint8_t>> SendOnStream(const std::vector<uint8_t> &payload, uint64_t nowMs)
	{
		if (m_streamId < 0 && ngtcp2_conn_open_bidi_stream(m_conn, &m_streamId, nullptr) != 0) {
			return {};
		}
		std::vector<std::vector<uint8_t>> out;
		const ngtcp2_tstamp ts = nowMs * UINT64_C(1000000);
		const ngtcp2_vec vec{ const_cast<uint8_t *>(payload.data()), payload.size() };
		bool dataSent = payload.empty();
		for (int round = 0; round < kMaxPacketsPerPump; ++round) {
			uint8_t buf[kMaxUdpPayload];
			ngtcp2_path_storage path;
			InitZeroPath(path);
			ngtcp2_pkt_info pi = {};
			ngtcp2_ssize dataLen = 0;
			const ngtcp2_ssize written = ngtcp2_conn_writev_stream(m_conn, &path.path, &pi, buf,
				sizeof(buf), &dataLen, 0, m_streamId, dataSent ? nullptr : &vec, dataSent ? 0 : 1, ts);
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

private:
	static constexpr size_t kMaxUdpPayload = 1452;
	static constexpr int kMaxPacketsPerPump = 16;

	gnutls_certificate_credentials_t m_credentials = nullptr;
	gnutls_session_t m_session = nullptr;
	ngtcp2_conn *m_conn = nullptr;
	ngtcp2_crypto_conn_ref m_connRef{};
	bool m_handshakeConfirmed = false;
	int64_t m_streamId = -1;
};
} // namespace

TEST(QuicNgtcp2Handshake, ARealClientAndTheRealServerEngineCompleteTheHandshake)
{
	CQuicEphemeralCredentials credentials;
	ASSERT_TRUE(credentials.NativeGnuTlsCredentials() != nullptr);
	ForeignSession session;
	ForeignVerifier verifier;
	CQuicTlsPolicy policy{ &session, &credentials, &verifier, &session };

	auto sink = std::make_shared<CollectingSink>();
	auto engine = CreateProductionQuicNgtcp2Engine();
	CQuicNgtcp2Factory factory(policy, sink, engine);

	CTestQuicClient client;
	ASSERT_TRUE(client.Init());

	// Not CQuicContext: it owns connections privately and exposes no way to reach one again,
	// which this test needs to drain the stream data it is about to prove arrives. There is
	// only one connection here, so the routing CQuicContext would otherwise do is unnecessary --
	// every datagram after the first just goes straight to it.
	std::unique_ptr<IQuicConnection> connection;
	bool confirmed = false;
	uint64_t nowMs = 0;
	// One handshake takes a handful of round trips; this bounds a genuine protocol failure to a
	// fast, deterministic test failure instead of a hang.
	constexpr int kMaxRounds = 20;
	for (int round = 0; round < kMaxRounds && !confirmed; ++round, nowMs += 10) {
		for (const auto &datagram : client.Pump(nowMs)) {
			if (!connection) {
				const std::string dcid = ExtractInitialDcid(datagram);
				ASSERT_TRUE(!dcid.empty());
				connection = factory.CreateInbound(datagram.data(), datagram.size(), kPeer, 4672, dcid);
				ASSERT_TRUE(connection != nullptr);
			}
			ASSERT_TRUE(connection->ProcessDatagram(datagram.data(), datagram.size(), nowMs));
		}
		for (const auto &datagram : sink->sent) {
			ASSERT_TRUE(client.Receive(datagram, nowMs));
		}
		sink->sent.clear();
		confirmed = client.HandshakeConfirmed();
	}

	ASSERT_TRUE(confirmed);
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
}
