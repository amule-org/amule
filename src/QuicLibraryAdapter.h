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

#ifndef QUICLIBRARYADAPTER_H
#define QUICLIBRARYADAPTER_H

#include "QuicNattProtocol.h"

#include <cstddef>
#include <cstdint>
#include <array>
#include <vector>

struct gnutls_session_int;
typedef gnutls_session_int *gnutls_session_t;

class IQuicLibraryCallbacks
{
public:
	virtual ~IQuicLibraryCallbacks() = default;
	virtual void OnQuicConnected() = 0;
	virtual void OnQuicPayload(const uint8_t *, size_t) = 0;
	virtual void OnQuicAuthenticationFailure() = 0;
};

class IQuicTlsCredentials
{
public:
	virtual ~IQuicTlsCredentials() = default;
};

class IQuicTlsVerifier
{
public:
	virtual ~IQuicTlsVerifier() = default;
};

class IQuicTlsSession
{
public:
	virtual ~IQuicTlsSession() = default;
	virtual bool ConfigureTls13Alpn(const IQuicTlsCredentials &,
		const IQuicTlsVerifier *,
		const uint8_t *,
		size_t,
		bool mandatory) = 0;
};

class IQuicNgtcp2TlsSession : public IQuicTlsSession
{
public:
	~IQuicNgtcp2TlsSession() override = default;
	virtual gnutls_session_t NativeGnuTlsSession() const = 0;
};

class IQuicLibrary
{
public:
	virtual ~IQuicLibrary() = default;
	virtual bool Configure() = 0;
	virtual bool OnHandshakeComplete(const uint8_t *, size_t) = 0;
	virtual bool OnStreamData(const uint8_t *, size_t) = 0;
};

struct CQuicTlsPolicy
{
	IQuicTlsSession *session = nullptr;
	const IQuicTlsCredentials *credentials = nullptr;
	const IQuicTlsVerifier *verifier = nullptr;
	IQuicNgtcp2TlsSession *ngtcp2Session = nullptr;
};

class CQuicLibraryAdapter final : public IQuicLibrary
{
public:
	CQuicLibraryAdapter(const CQuicTlsPolicy &,
		const std::array<uint8_t, 16> &,
		const std::array<uint8_t, 16> *,
		IQuicLibraryCallbacks *);
	bool Configure() override;
	bool OnHandshakeComplete(const uint8_t *, size_t) override;
	bool OnStreamData(const uint8_t *, size_t) override;
	bool IsReady() const { return m_ready; }

private:
	CQuicTlsPolicy m_policy;
	const std::array<uint8_t, 16> m_localIdentity;
	const std::array<uint8_t, 16> *m_expectedPeerIdentity;
	IQuicLibraryCallbacks *m_callbacks;
	bool m_configured = false;
	bool m_ready = false;
	bool m_handshake = false;
	bool m_authenticationReported = false;
	std::vector<uint8_t> m_proof;
};

#endif
