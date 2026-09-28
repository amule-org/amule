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

#include "QuicContext.h"

#include "PeerAddressing.h"

#include <iterator>

CQuicContext::~CQuicContext()
{
	for (auto &entry : m_connections) {
		entry.second->Close();
	}
}

bool CQuicContext::ClassifyInitial(const uint8_t *payload, size_t length, std::string &key)
{
	// Only routing: CreateInbound() applies the full RFC 9000 acceptance rules.
	if (!payload || length < kMinInitialDatagram || (payload[0] & 0xC0) != 0xC0 ||
		(payload[0] & 0x30) != 0 || (payload[1] | payload[2] | payload[3] | payload[4]) == 0) {
		return false;
	}
	const size_t cidLength = payload[5];
	if (cidLength == 0 || cidLength > 20 || length < 6 + cidLength) {
		return false;
	}
	key.assign(reinterpret_cast<const char *>(payload + 6), cidLength);
	return true;
}

CQuicContext::ConnectionMap::iterator CQuicContext::FindSoleEndpointConnection(
	const CNetworkAddress &address, uint16_t port)
{
	auto first = m_connections.lower_bound(SConnectionKey{ address, port, std::string() });
	if (first == m_connections.end() || first->first.address != address || first->first.port != port) {
		return m_connections.end();
	}
	auto next = std::next(first);
	if (next != m_connections.end() && next->first.address == address && next->first.port == port) {
		return m_connections.end();
	}
	return first;
}

CQuicContext::ConnectionMap::iterator CQuicContext::FindInitialConnection(
	const CNetworkAddress &address, uint16_t port, const std::string &cid)
{
	auto exact = m_connections.find(SConnectionKey{ address, port, cid });
	if (exact != m_connections.end()) {
		return exact;
	}
	for (auto entry = m_connections.lower_bound(SConnectionKey{ address, port, std::string() });
		entry != m_connections.end() && entry->first.address == address && entry->first.port == port;
		++entry) {
		if (entry->second->OwnsConnectionId(cid)) {
			return entry;
		}
	}
	return m_connections.end();
}

void CQuicContext::EraseClosedConnections()
{
	for (auto entry = m_connections.begin(); entry != m_connections.end();) {
		if (entry->second->IsClosed()) {
			entry = m_connections.erase(entry);
		} else {
			++entry;
		}
	}
}

bool CQuicContext::AdmitNewConnection(const CNetworkAddress &address, uint64_t nowMs)
{
	EraseClosedConnections();
	if (m_connections.size() >= kMaxConnections) {
		return false;
	}
	const CNetworkAddress scope = PeerAddressing::RateLimitScope(address);
	size_t inScope = 0;
	for (const auto &entry : m_connections) {
		if (PeerAddressing::RateLimitScope(entry.first.address) == scope &&
			++inScope >= kMaxConnectionsPerScope) {
			return false;
		}
	}
	// Counted before CreateInbound(): refused attempts must cost the sender as much as accepted ones.
	return m_newConnectionLimiter.Admit(address, nowMs);
}

bool CQuicContext::ProcessDatagram(
	const uint8_t *payload, size_t length, const CNetworkAddress &address, uint16_t port, uint64_t nowMs)
{
	if (!payload || length == 0) {
		return false;
	}

	std::string cid;
	const bool initial = ClassifyInitial(payload, length, cid);
	auto existing = initial ? FindInitialConnection(address, port, cid)
				: FindSoleEndpointConnection(address, port);
	if (existing != m_connections.end()) {
		const bool accepted = existing->second->ProcessDatagram(payload, length);
		if (!accepted || existing->second->IsClosed()) {
			existing->second->Close();
			m_connections.erase(existing);
		}
		return accepted;
	}
	if (!initial || !m_factory || !AdmitNewConnection(address, nowMs)) {
		return false;
	}
	std::unique_ptr<IQuicConnection> connection =
		m_factory->CreateInbound(payload, length, address, port, cid);
	if (!connection) {
		return false;
	}
	const bool accepted = connection->ProcessDatagram(payload, length);
	if (!accepted || connection->IsClosed()) {
		connection->Close();
		return accepted;
	}
	m_connections.emplace(SConnectionKey{ address, port, cid }, std::move(connection));
	return true;
}
