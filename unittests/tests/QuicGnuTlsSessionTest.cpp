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

#include <muleunit/test.h>
#include <QuicGnuTlsSession.h>
#include <QuicNattProtocol.h>

#include <gnutls/gnutls.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <deque>
#include <string>

using namespace muleunit;
DECLARE_SIMPLE(QuicGnuTlsSession)

namespace
{
struct NoopVerifier : IQuicTlsVerifier
{
};

// gnutls_global_init() is reference-counted, and CQuicEphemeralCredentials/CQuicGnuTlsSession
// call it exactly once (via a std::call_once guard) no matter how many are constructed in this
// process. Production code never calls gnutls_global_deinit(): a long-running daemon should not
// release a process-wide cache it may need again. This test binary is not that daemon, so it
// balances that one init with one deinit at exit -- otherwise LeakSanitizer reports GnuTLS's and
// libtasn1's one-time global ASN.1/crypto-backend state as a leak on every run.
struct GnuTlsGlobalDeinitOnExit final
{
	~GnuTlsGlobalDeinitOnExit() { gnutls_global_deinit(); }
} kGnuTlsGlobalDeinitOnExit;

// The client side of these tests is a plain GnuTLS session, not CQuicGnuTlsSession: it stands in
// for a QUIC peer's TLS stack, which this codebase does not implement. It accepts any server
// certificate because this protocol authenticates the peer via the EAQN1 proof, not the PKI --
// matching what CQuicGnuTlsSession itself does not request from its side either.
int AcceptAnyCertificate(gnutls_session_t)
{
	return 0;
}

struct ScopedClientSession
{
	gnutls_certificate_credentials_t credentials = nullptr;
	gnutls_session_t session = nullptr;

	~ScopedClientSession()
	{
		if (session != nullptr) {
			gnutls_deinit(session);
		}
		if (credentials != nullptr) {
			gnutls_certificate_free_credentials(credentials);
		}
	}
};

// One side of an in-memory, non-blocking byte stream between the two sessions. It stands in for a
// socketpair, which Windows does not have.
struct MemoryEndpoint
{
	gnutls_session_t session = nullptr;
	std::deque<unsigned char> *incoming = nullptr;
	std::deque<unsigned char> *outgoing = nullptr;
};

ssize_t MemoryPush(gnutls_transport_ptr_t ptr, const void *data, size_t size)
{
	auto *endpoint = static_cast<MemoryEndpoint *>(ptr);
	const auto *bytes = static_cast<const unsigned char *>(data);
	endpoint->outgoing->insert(endpoint->outgoing->end(), bytes, bytes + size);
	return static_cast<ssize_t>(size);
}

// An empty stream reads as EAGAIN, as a non-blocking socket would: the handshake returns
// GNUTLS_E_AGAIN and the pump loop gives the other side its turn.
ssize_t MemoryPull(gnutls_transport_ptr_t ptr, void *data, size_t size)
{
	auto *endpoint = static_cast<MemoryEndpoint *>(ptr);
	if (endpoint->incoming->empty()) {
		gnutls_transport_set_errno(endpoint->session, EAGAIN);
		return -1;
	}
	const auto count = static_cast<std::ptrdiff_t>(std::min(size, endpoint->incoming->size()));
	const auto end = endpoint->incoming->begin() + count;
	std::copy(endpoint->incoming->begin(), end, static_cast<unsigned char *>(data));
	endpoint->incoming->erase(endpoint->incoming->begin(), end);
	return static_cast<ssize_t>(count);
}

void AttachMemoryTransport(MemoryEndpoint &endpoint)
{
	gnutls_transport_set_ptr(endpoint.session, &endpoint);
	gnutls_transport_set_push_function(endpoint.session, MemoryPush);
	gnutls_transport_set_pull_function(endpoint.session, MemoryPull);
}

struct HandshakeResult
{
	bool ok = false;
	gnutls_protocol_t protocol = GNUTLS_VERSION_UNKNOWN;
	std::string negotiatedAlpn;
};

// Drives a real handshake over an in-memory, non-blocking transport: one real GnuTLS client and
// CQuicGnuTlsSession as the server. Both sides are pumped from this one thread so completion
// depends only on protocol progress, never on which OS thread the kernel happens to schedule
// next -- the failure mode a two-thread, blocking-socket version of this test would have instead.
HandshakeResult RunHandshakeAgainstServer(CQuicGnuTlsSession &server, const std::string &clientAlpn)
{
	HandshakeResult result;

	ScopedClientSession client;
	if (gnutls_certificate_allocate_credentials(&client.credentials) != GNUTLS_E_SUCCESS) {
		return result;
	}
	gnutls_certificate_set_verify_function(client.credentials, AcceptAnyCertificate);
	if (gnutls_init(&client.session, GNUTLS_CLIENT | GNUTLS_NONBLOCK) != GNUTLS_E_SUCCESS) {
		return result;
	}
	gnutls_priority_set_direct(client.session, "NORMAL:-VERS-ALL:+VERS-TLS1.3", nullptr);
	gnutls_credentials_set(client.session, GNUTLS_CRD_CERTIFICATE, client.credentials);
	const gnutls_datum_t alpn{ const_cast<unsigned char *>(
					   reinterpret_cast<const unsigned char *>(clientAlpn.data())),
		static_cast<unsigned>(clientAlpn.size()) };
	gnutls_alpn_set_protocols(client.session, &alpn, 1, 0);
	std::deque<unsigned char> toServer;
	std::deque<unsigned char> toClient;
	MemoryEndpoint clientEnd{ client.session, &toClient, &toServer };
	MemoryEndpoint serverEnd{ server.NativeGnuTlsSession(), &toServer, &toClient };
	AttachMemoryTransport(clientEnd);
	AttachMemoryTransport(serverEnd);

	bool clientDone = false;
	bool clientOk = false;
	bool serverDone = false;
	bool serverOk = false;
	// One handshake flight is a handful of round trips; this bounds a genuine protocol failure
	// to a fast, deterministic test failure instead of a hang if something is actually broken.
	constexpr int kMaxRounds = 200;
	for (int round = 0; round < kMaxRounds && (!clientDone || !serverDone); ++round) {
		if (!clientDone) {
			const int rv = gnutls_handshake(client.session);
			if (rv == GNUTLS_E_SUCCESS) {
				clientDone = true;
				clientOk = true;
			} else if (gnutls_error_is_fatal(rv) != 0) {
				clientDone = true;
			}
		}
		if (!serverDone) {
			const int rv = gnutls_handshake(server.NativeGnuTlsSession());
			if (rv == GNUTLS_E_SUCCESS) {
				serverDone = true;
				serverOk = true;
			} else if (gnutls_error_is_fatal(rv) != 0) {
				serverDone = true;
			}
		}
	}

	if (clientOk && serverOk) {
		result.ok = true;
		result.protocol = gnutls_protocol_get_version(client.session);
		gnutls_datum_t selected{};
		if (gnutls_alpn_get_selected_protocol(client.session, &selected) == GNUTLS_E_SUCCESS) {
			result.negotiatedAlpn.assign(
				reinterpret_cast<const char *>(selected.data), selected.size);
		}
	}
	return result;
}
} // namespace

TEST(QuicGnuTlsSession, GeneratesUsableCertificateCredentials)
{
	CQuicEphemeralCredentials credentials;
	ASSERT_TRUE(credentials.NativeGnuTlsCredentials() != nullptr);
}

TEST(QuicGnuTlsSession, RejectsAForeignCredentialsType)
{
	struct ForeignCredentials : IQuicTlsCredentials
	{
	};
	ForeignCredentials foreign;
	CQuicGnuTlsSession session;
	NoopVerifier verifier;
	const auto *alpn = reinterpret_cast<const uint8_t *>(QuicNatt::QUIC_NATT_ALPN);
	ASSERT_TRUE(!session.ConfigureTls13Alpn(
		foreign, &verifier, alpn, sizeof(QuicNatt::QUIC_NATT_ALPN) - 1, true));
}

TEST(QuicGnuTlsSession, NegotiatesTls13AndTheExactAlpn)
{
	CQuicEphemeralCredentials credentials;
	CQuicGnuTlsSession server;
	NoopVerifier verifier;
	const auto *alpn = reinterpret_cast<const uint8_t *>(QuicNatt::QUIC_NATT_ALPN);
	ASSERT_TRUE(server.ConfigureTls13Alpn(
		credentials, &verifier, alpn, sizeof(QuicNatt::QUIC_NATT_ALPN) - 1, true));

	const HandshakeResult result = RunHandshakeAgainstServer(server, QuicNatt::QUIC_NATT_ALPN);
	ASSERT_TRUE(result.ok);
	ASSERT_EQUALS(static_cast<int>(GNUTLS_TLS1_3), static_cast<int>(result.protocol));
	ASSERT_EQUALS(std::string(QuicNatt::QUIC_NATT_ALPN), result.negotiatedAlpn);
}

TEST(QuicGnuTlsSession, RejectsAWrongAlpnAsMandatory)
{
	CQuicEphemeralCredentials credentials;
	CQuicGnuTlsSession server;
	NoopVerifier verifier;
	const auto *alpn = reinterpret_cast<const uint8_t *>(QuicNatt::QUIC_NATT_ALPN);
	ASSERT_TRUE(server.ConfigureTls13Alpn(
		credentials, &verifier, alpn, sizeof(QuicNatt::QUIC_NATT_ALPN) - 1, true));

	const HandshakeResult result = RunHandshakeAgainstServer(server, "h3");
	ASSERT_TRUE(!result.ok);
}
