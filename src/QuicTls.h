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

#ifndef QUICTLS_H
#define QUICTLS_H

#include <cstddef>
#include <cstdint>
#include <memory>

struct gnutls_session_int;
typedef gnutls_session_int *gnutls_session_t;

struct gnutls_certificate_credentials_st;
typedef gnutls_certificate_credentials_st *gnutls_certificate_credentials_t;

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

class IQuicGnuTlsCredentials : public IQuicTlsCredentials
{
public:
	~IQuicGnuTlsCredentials() override = default;
	virtual gnutls_certificate_credentials_t NativeGnuTlsCredentials() const = 0;
};

//! The real server certificate and key, shared across every connection this process creates.
std::shared_ptr<IQuicTlsCredentials> CreateProductionQuicCredentials();

#endif
