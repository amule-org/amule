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

#include "QuicLibraryAdapter.h"

CQuicLibraryAdapter::CQuicLibraryAdapter(const CQuicTlsPolicy &policy,
	const std::array<uint8_t, 16> &localIdentity,
	const std::array<uint8_t, 16> *expectedPeerIdentity,
	IQuicLibraryCallbacks *callbacks)
: m_policy(policy)
, m_localIdentity(localIdentity)
, m_expectedPeerIdentity(expectedPeerIdentity)
, m_callbacks(callbacks)
{
}

bool CQuicLibraryAdapter::Configure()
{
	if (m_policy.session == nullptr || m_policy.credentials == nullptr || m_policy.verifier == nullptr)
		return false;
	const auto *alpn = reinterpret_cast<const uint8_t *>(QuicNatt::QUIC_NATT_ALPN);
	const size_t alpnLength = sizeof(QuicNatt::QUIC_NATT_ALPN) - 1;
	m_configured = m_policy.session->ConfigureTls13Alpn(
		*m_policy.credentials, m_policy.verifier, alpn, alpnLength, true);
	return m_configured;
}

bool CQuicLibraryAdapter::OnHandshakeComplete(const uint8_t *selectedAlpn, size_t length)
{
	if (!m_configured || !QuicNatt::IsQuicNattAlpn(selectedAlpn, length))
		return false;
	m_handshake = true;
	return true;
}

bool CQuicLibraryAdapter::OnStreamData(const uint8_t *data, size_t length)
{
	if (!m_handshake || data == nullptr || length == 0)
		return false;
	if (m_ready) {
		if (m_callbacks != nullptr)
			m_callbacks->OnQuicPayload(data, length);
		return true;
	}

	m_proof.insert(m_proof.end(), data, data + length);
	if (m_proof.size() >= 5 && (m_proof[0] != 'E' || m_proof[1] != 'A' || m_proof[2] != 'Q' ||
					   m_proof[3] != 'N' || m_proof[4] != '1')) {
		if (!m_authenticationReported && m_callbacks != nullptr) {
			m_authenticationReported = true;
			m_callbacks->OnQuicAuthenticationFailure();
		}
		m_proof.clear();
		return false;
	}
	if (m_proof.size() < QuicNatt::EAQN1_PROOF_SIZE)
		return true;

	if (!QuicNatt::ValidateEaqn1Proof(
		    m_proof.data(), m_proof.size(), m_localIdentity, m_expectedPeerIdentity)) {
		if (!m_authenticationReported && m_callbacks != nullptr) {
			m_authenticationReported = true;
			m_callbacks->OnQuicAuthenticationFailure();
		}
		m_proof.clear();
		return false;
	}

	m_ready = true;
	if (m_callbacks != nullptr)
		m_callbacks->OnQuicConnected();
	const size_t payloadOffset = QuicNatt::EAQN1_PROOF_SIZE;
	if (m_callbacks != nullptr && m_proof.size() > payloadOffset)
		m_callbacks->OnQuicPayload(m_proof.data() + payloadOffset, m_proof.size() - payloadOffset);
	m_proof.clear();
	return true;
}
