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

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

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

bool SetNonBlocking(int fd)
{
	const int flags = fcntl(fd, F_GETFL, 0);
	return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

struct HandshakeResult
{
	bool ok = false;
	gnutls_protocol_t protocol = GNUTLS_VERSION_UNKNOWN;
	std::string negotiatedAlpn;
};

// Drives a real handshake over a real (local) transport: a non-blocking socketpair, one real
// GnuTLS client and CQuicGnuTlsSession as the server. Both sides are pumped from this one
// thread so completion depends only on protocol progress, never on which OS thread the kernel
// happens to schedule next -- the failure mode a two-thread, blocking-socket version of this
// test would have instead.
HandshakeResult RunHandshakeAgainstServer(CQuicGnuTlsSession &server, const std::string &clientAlpn)
{
	HandshakeResult result;

	int fds[2] = { -1, -1 };
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
		return result;
	}
	if (!SetNonBlocking(fds[0]) || !SetNonBlocking(fds[1])) {
		close(fds[0]);
		close(fds[1]);
		return result;
	}

	ScopedClientSession client;
	if (gnutls_certificate_allocate_credentials(&client.credentials) != GNUTLS_E_SUCCESS) {
		close(fds[0]);
		close(fds[1]);
		return result;
	}
	gnutls_certificate_set_verify_function(client.credentials, AcceptAnyCertificate);
	if (gnutls_init(&client.session, GNUTLS_CLIENT | GNUTLS_NONBLOCK) != GNUTLS_E_SUCCESS) {
		close(fds[0]);
		close(fds[1]);
		return result;
	}
	gnutls_priority_set_direct(client.session, "NORMAL:-VERS-ALL:+VERS-TLS1.3", nullptr);
	gnutls_credentials_set(client.session, GNUTLS_CRD_CERTIFICATE, client.credentials);
	const gnutls_datum_t alpn{ const_cast<unsigned char *>(
					   reinterpret_cast<const unsigned char *>(clientAlpn.data())),
		static_cast<unsigned>(clientAlpn.size()) };
	gnutls_alpn_set_protocols(client.session, &alpn, 1, 0);
	gnutls_transport_set_int(client.session, fds[0]);
	gnutls_transport_set_int(server.NativeGnuTlsSession(), fds[1]);

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

	close(fds[0]);
	close(fds[1]);

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
