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

#ifndef QUICGNUTLSSESSION_H
#define QUICGNUTLSSESSION_H

#include "QuicTls.h"

#include <gnutls/gnutls.h>

/**
 * Owns a self-signed, ephemeral ECDSA P-256 certificate and key for the QUIC NAT-T TLS server
 * identity. Generated once in memory per process and never persisted to disk or reused across
 * restarts.
 *
 * The certificate CN is "amule", deliberately distinct from eMuleAI's own certificate. Neither
 * side validates the other's certificate: peer identity is established by the EAQN1 proof
 * (QuicNattProtocol.h), which travels over the encrypted stream once the handshake completes.
 * A certificate is still required because TLS 1.3 has no anonymous server-authentication mode;
 * this one exists to make the handshake possible, not to identify anyone.
 */
class CQuicEphemeralCredentials final : public IQuicGnuTlsCredentials
{
public:
	CQuicEphemeralCredentials();
	~CQuicEphemeralCredentials() override;
	CQuicEphemeralCredentials(const CQuicEphemeralCredentials &) = delete;
	CQuicEphemeralCredentials &operator=(const CQuicEphemeralCredentials &) = delete;

	//! Null if generation failed; callers must check before use.
	gnutls_certificate_credentials_t NativeGnuTlsCredentials() const override { return m_credentials; }

private:
	gnutls_certificate_credentials_t m_credentials = nullptr;
};

/**
 * A real GnuTLS server session: TLS 1.3 only, with the exact mandatory ALPN the caller supplies.
 *
 * Requests no client certificate. This class only configures the session; it does not drive a
 * handshake or bind it to a QUIC connection -- see the ngtcp2 adapter for that, which is a
 * separate, larger piece of work this class deliberately does not attempt.
 */
class CQuicGnuTlsSession final : public IQuicNgtcp2TlsSession
{
public:
	CQuicGnuTlsSession();
	~CQuicGnuTlsSession() override;
	CQuicGnuTlsSession(const CQuicGnuTlsSession &) = delete;
	CQuicGnuTlsSession &operator=(const CQuicGnuTlsSession &) = delete;

	bool ConfigureTls13Alpn(const IQuicTlsCredentials &,
		const IQuicTlsVerifier *,
		const uint8_t *,
		size_t,
		bool mandatory) override;
	gnutls_session_t NativeGnuTlsSession() const override { return m_session; }

private:
	gnutls_session_t m_session = nullptr;
};

#endif
