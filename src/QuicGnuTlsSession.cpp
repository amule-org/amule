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

#include <gnutls/crypto.h>
#include <gnutls/x509.h>

#include <ctime>
#include <mutex>

namespace
{
// GnuTLS is not thread-safe until this has run at least once, and must run before any thread
// that touches GnuTLS is created (GnuTLS manual, gnutls_global_init()). Every production and
// test entry point into this file goes through one of the two constructors below, so guarding
// it here -- once, before either does anything else -- covers all of them.
void EnsureGnuTlsInitialized()
{
	static std::once_flag flag;
	std::call_once(flag, [] { gnutls_global_init(); });
}

constexpr unsigned kSerialBytes = 8;
constexpr long kValidityDays = 365;
constexpr long kSecondsPerDay = 24 * 60 * 60;

// gnutls_certificate_set_x509_key() does not take ownership of either argument -- measured
// under LeakSanitizer, not assumed: both must be deinitialized after the call whether it
// succeeds or fails, and this function is responsible for freeing whatever it already
// allocated on any earlier failure too.
gnutls_certificate_credentials_t GenerateEphemeralCredentials()
{
	gnutls_x509_privkey_t key = nullptr;
	if (gnutls_x509_privkey_init(&key) != GNUTLS_E_SUCCESS) {
		return nullptr;
	}
	if (gnutls_x509_privkey_generate(
		    key, GNUTLS_PK_ECDSA, GNUTLS_CURVE_TO_BITS(GNUTLS_ECC_CURVE_SECP256R1), 0) !=
		GNUTLS_E_SUCCESS) {
		gnutls_x509_privkey_deinit(key);
		return nullptr;
	}

	gnutls_x509_crt_t certificate = nullptr;
	if (gnutls_x509_crt_init(&certificate) != GNUTLS_E_SUCCESS) {
		gnutls_x509_privkey_deinit(key);
		return nullptr;
	}

	uint8_t serial[kSerialBytes];
	const time_t now = time(nullptr);
	const bool built =
		gnutls_x509_crt_set_version(certificate, 3) == GNUTLS_E_SUCCESS &&
		gnutls_x509_crt_set_key(certificate, key) == GNUTLS_E_SUCCESS &&
		gnutls_rnd(GNUTLS_RND_NONCE, serial, sizeof(serial)) == GNUTLS_E_SUCCESS &&
		gnutls_x509_crt_set_serial(certificate, serial, sizeof(serial)) == GNUTLS_E_SUCCESS &&
		// Backdated by a day to tolerate clock skew with a peer that checked activation time,
		// even though neither side actually validates this certificate (see the class comment).
		gnutls_x509_crt_set_activation_time(certificate, now - kSecondsPerDay) == GNUTLS_E_SUCCESS &&
		gnutls_x509_crt_set_expiration_time(certificate, now + kValidityDays * kSecondsPerDay) ==
			GNUTLS_E_SUCCESS &&
		gnutls_x509_crt_set_dn(certificate, "CN=amule", nullptr) == GNUTLS_E_SUCCESS &&
		gnutls_x509_crt_sign2(certificate, certificate, key, GNUTLS_DIG_SHA256, 0) ==
			GNUTLS_E_SUCCESS;
	if (!built) {
		gnutls_x509_crt_deinit(certificate);
		gnutls_x509_privkey_deinit(key);
		return nullptr;
	}

	gnutls_certificate_credentials_t credentials = nullptr;
	if (gnutls_certificate_allocate_credentials(&credentials) != GNUTLS_E_SUCCESS) {
		gnutls_x509_crt_deinit(certificate);
		gnutls_x509_privkey_deinit(key);
		return nullptr;
	}
	const int setKeyResult = gnutls_certificate_set_x509_key(credentials, &certificate, 1, key);
	gnutls_x509_crt_deinit(certificate);
	gnutls_x509_privkey_deinit(key);
	if (setKeyResult != GNUTLS_E_SUCCESS) {
		gnutls_certificate_free_credentials(credentials);
		return nullptr;
	}
	return credentials;
}
} // namespace

CQuicEphemeralCredentials::CQuicEphemeralCredentials()
{
	EnsureGnuTlsInitialized();
	m_credentials = GenerateEphemeralCredentials();
}

CQuicEphemeralCredentials::~CQuicEphemeralCredentials()
{
	if (m_credentials != nullptr) {
		gnutls_certificate_free_credentials(m_credentials);
	}
}

CQuicGnuTlsSession::CQuicGnuTlsSession()
{
	EnsureGnuTlsInitialized();
	// Server-only for this slice: CQuicNgtcp2Factory only ever creates inbound connections, and
	// an outbound QUIC dial does not exist yet.
	if (gnutls_init(&m_session, GNUTLS_SERVER) != GNUTLS_E_SUCCESS) {
		m_session = nullptr;
	}
}

CQuicGnuTlsSession::~CQuicGnuTlsSession()
{
	if (m_session != nullptr) {
		gnutls_deinit(m_session);
	}
}

bool CQuicGnuTlsSession::ConfigureTls13Alpn(const IQuicTlsCredentials &credentials,
	const IQuicTlsVerifier *,
	const uint8_t *alpn,
	size_t alpnLength,
	bool mandatory)
{
	if (m_session == nullptr) {
		return false;
	}
	// The abstract IQuicTlsCredentials parameter exists so non-GnuTLS test doubles can stand
	// in for it elsewhere; this session only ever pairs with a real GnuTLS credentials object.
	const auto *gnutlsCredentials = dynamic_cast<const IQuicGnuTlsCredentials *>(&credentials);
	if (gnutlsCredentials == nullptr || gnutlsCredentials->NativeGnuTlsCredentials() == nullptr) {
		return false;
	}
	if (gnutls_priority_set_direct(m_session, "NORMAL:-VERS-ALL:+VERS-TLS1.3", nullptr) !=
		GNUTLS_E_SUCCESS) {
		return false;
	}
	if (gnutls_credentials_set(m_session,
		    GNUTLS_CRD_CERTIFICATE,
		    gnutlsCredentials->NativeGnuTlsCredentials()) != GNUTLS_E_SUCCESS) {
		return false;
	}
	const gnutls_datum_t protocol{ const_cast<unsigned char *>(
					       reinterpret_cast<const unsigned char *>(alpn)),
		static_cast<unsigned>(alpnLength) };
	const unsigned flags = mandatory ? GNUTLS_ALPN_MANDATORY : 0;
	if (gnutls_alpn_set_protocols(m_session, &protocol, 1, flags) != GNUTLS_E_SUCCESS) {
		return false;
	}
	// No client certificate is requested: see the class comment on peer identity.
	return true;
}

std::shared_ptr<IQuicTlsCredentials> CreateProductionQuicCredentials()
{
	return std::make_shared<CQuicEphemeralCredentials>();
}
