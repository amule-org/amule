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

#ifndef AMULE_NAT_SOURCE_ECHO_H
#define AMULE_NAT_SOURCE_ECHO_H

#include "NatKeyFrame.h"                // Needed for the bounded-map helpers and CSendGate
#include "include/protocol/Protocols.h" // Needed for the protocol bytes

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>

/**
 * The ed2k client UDP key mixes in the sender's public IPv4, and the receiver takes that IPv4 from
 * the datagram's source. Behind a NAT that sends UDP from another public IPv4 than TCP, the two
 * differ, so the peer cannot decode our obfuscated datagrams. A key IP is the IPv4 one peer sees
 * our UDP at; the server's own-endpoint field in 0x61 and the peer's source echo supply it.
 *
 * IPv4 values are aMule's network-order uint32, as the rest of the client UDP code holds them.
 */
namespace NatSourceEcho
{
constexpr std::size_t kPayloadBytes = 6;
//! CRYPT_HEADER_WITHOUTPADDING: a shorter datagram is never tried as obfuscated.
constexpr std::size_t kCryptHeaderBytes = 8;
//! A transfer over a punched path can run for hours, so a key IP ends only after an idle hour.
constexpr std::uint64_t kKeyIpIdleMs = 3600000;
constexpr std::size_t kMaxKeyIps = 256;
//! The punch window, and one echo per source within it.
constexpr std::uint64_t kWindowMs = NatKeyFrame::kExpectedPeerTtlMs;
constexpr std::size_t kMaxPunchedPeers = 256;

using Hash = NatKeyFrame::Hash;

struct Source
{
	std::uint32_t ip;
	std::uint16_t port;
};

//! <ip 4, as in 0x61><port 2 LE>.
inline std::array<std::uint8_t, kPayloadBytes> BuildPayload(const Source &source)
{
	return { static_cast<std::uint8_t>(source.ip),
		static_cast<std::uint8_t>(source.ip >> 8),
		static_cast<std::uint8_t>(source.ip >> 16),
		static_cast<std::uint8_t>(source.ip >> 24),
		static_cast<std::uint8_t>(source.port),
		static_cast<std::uint8_t>(source.port >> 8) };
}

//! Surplus bytes are ignored, so the payload can grow.
inline std::optional<Source> ParsePayload(const std::uint8_t *payload, std::size_t length)
{
	if (payload == nullptr || length < kPayloadBytes) {
		return std::nullopt;
	}
	const std::uint32_t ip = static_cast<std::uint32_t>(payload[0]) |
				 (static_cast<std::uint32_t>(payload[1]) << 8) |
				 (static_cast<std::uint32_t>(payload[2]) << 16) |
				 (static_cast<std::uint32_t>(payload[3]) << 24);
	const std::uint16_t port = static_cast<std::uint16_t>(payload[4] | (payload[5] << 8));
	return Source{ ip, port };
}

//! Whether CEncryptedDatagramSocket::DecryptReceivedClient() tried every key on a datagram and none
//! fitted: it leaves the buffer as it was, and passes the plain protocol bytes through untried.
inline bool FailedEveryKey(std::uint8_t firstByte, std::size_t length, bool decrypted)
{
	if (decrypted || length <= kCryptHeaderBytes) {
		return false;
	}
	switch (firstByte) {
	case OP_EMULEPROT:
	case OP_KADEMLIAPACKEDPROT:
	case OP_KADEMLIAHEADER:
	case OP_UDPRESERVEDPROT1:
	case OP_UDPRESERVEDPROT2:
	case OP_PACKEDPROT:
		return false;
	default:
		return true;
	}
}

/**
 * Endpoints a rendezvous had us punch in the last 30 s, with the hash it vouched for. A NAT may move
 * the source port, so a datagram from another port still earns an echo when its IP has one punch.
 * At most one echo per window to each punched endpoint, and one for all the other ports of its IP:
 * matching on a moved port accepts every one of them.
 */
class CPunchedPeers
{
public:
	explicit CPunchedPeers(std::size_t capacity)
	: m_expected(capacity, kWindowMs)
	, m_echoGate(capacity, kWindowMs)
	{
	}

	//! Two different hashes vouched for one endpoint leave it with none.
	void Punched(
		std::uint32_t ip, std::uint16_t port, const std::optional<Hash> &hash, std::uint64_t nowMs)
	{
		m_expected.Expect(ip, port, nowMs, hash);
	}

	//! Exact endpoint only: an echo sets the key IP for the whole IP, so a neighbour behind the
	//! same carrier NAT must not be able to send it.
	bool IsPunched(std::uint32_t ip, std::uint16_t port, std::uint64_t nowMs) const
	{
		return m_expected.IsExpected(ip, port, nowMs);
	}

	//! The hash to obfuscate an echo to this source with, when one is due now.
	std::optional<Hash> TakeEcho(std::uint32_t ip,
		std::uint16_t port,
		std::uint8_t firstByte,
		std::size_t length,
		bool decrypted,
		std::uint64_t nowMs)
	{
		if (!FailedEveryKey(firstByte, length, decrypted)) {
			return std::nullopt;
		}
		const std::optional<Hash> hash = m_expected.VouchedHashAllowingMovedPort(ip, port, nowMs);
		if (!hash || !m_echoGate.TryAcquire(ip, GatePort(ip, port, nowMs), nowMs)) {
			return std::nullopt;
		}
		return hash;
	}

	//! The echo was not sent after all, so the next failed datagram may try again.
	void EchoNotSent(std::uint32_t ip, std::uint16_t port, std::uint64_t nowMs)
	{
		m_echoGate.Forget(ip, GatePort(ip, port, nowMs));
	}

private:
	//! A punched endpoint has its own gate; port 0 stands for every moved port of the IP.
	std::uint16_t GatePort(std::uint32_t ip, std::uint16_t port, std::uint64_t nowMs) const
	{
		return m_expected.IsExpected(ip, port, nowMs) ? port : 0;
	}

	NatKeyFrame::CExpectedPeers m_expected;
	NatKeyFrame::CSendGate m_echoGate;
};

//! Whether an echo from this source may set our key IP toward it. The echo is always sent
//! obfuscated, so a plain one is not the peer's.
inline bool AcceptsEcho(const CPunchedPeers &punched,
	std::uint32_t ip,
	std::uint16_t port,
	bool seenIsPublic,
	bool obfuscated,
	std::uint64_t nowMs)
{
	return obfuscated && seenIsPublic && punched.IsPunched(ip, port, nowMs);
}

//! Key IPs by peer IPv4, each for the login address it was learned under.
class CKeyIps
{
public:
	enum class From
	{
		Server, //!< The server's own-endpoint field: what most peers see.
		Peer    //!< The peer's echo: what this peer sees, so a server value never replaces it.
	};

	CKeyIps(std::size_t capacity, std::uint64_t idleMs)
	: m_capacity(capacity)
	, m_idleMs(idleMs)
	{
	}

	//! A key IP equal to our login address, or 0, removes the entry: the default key is right.
	//! Between server logins (login address 0) nothing changes.
	void
	Set(std::uint32_t peerIp, std::uint32_t keyIp, std::uint32_t loginIp, From from, std::uint64_t nowMs)
	{
		auto existing = m_entries.find(peerIp);
		if (existing != m_entries.end() && from == From::Server &&
			existing->second.from == From::Peer && existing->second.loginIp == loginIp &&
			!NatKeyFrame::Detail::Elapsed(nowMs, existing->second.stamp, m_idleMs)) {
			return;
		}
		if (loginIp == 0) {
			return;
		}
		if (keyIp == 0 || keyIp == loginIp) {
			if (existing != m_entries.end()) {
				m_entries.erase(existing);
			}
			return;
		}
		if (m_capacity == 0) {
			return;
		}
		if (existing == m_entries.end()) {
			NatKeyFrame::Detail::MakeRoom(m_entries, m_capacity, nowMs, m_idleMs);
		}
		m_entries[peerIp] = Entry{ keyIp, loginIp, nowMs, from };
	}

	//! A hit counts as use and extends the entry's life. An entry learned under another login
	//! address is dropped: our public IP changed, so what the peer sees may have too.
	std::optional<std::uint32_t> Lookup(std::uint32_t peerIp, std::uint32_t loginIp, std::uint64_t nowMs)
	{
		auto found = m_entries.find(peerIp);
		if (found == m_entries.end() || loginIp == 0) {
			return std::nullopt;
		}
		if (found->second.loginIp != loginIp ||
			NatKeyFrame::Detail::Elapsed(nowMs, found->second.stamp, m_idleMs)) {
			m_entries.erase(found);
			return std::nullopt;
		}
		found->second.stamp = nowMs;
		return found->second.keyIp;
	}

	std::size_t Size() const { return m_entries.size(); }

private:
	struct Entry
	{
		std::uint32_t keyIp;
		std::uint32_t loginIp;
		std::uint64_t stamp;
		From from;
	};

	std::size_t m_capacity;
	std::uint64_t m_idleMs;
	std::map<std::uint32_t, Entry> m_entries;
};

//! Our address in the uTP simultaneous-open order, as the peer sees our SYN's source: the key IP
//! toward it, else Kad's view, which the external Kad port comes from, else our public IP.
inline std::uint32_t LocalUdpIP(std::uint32_t keyIp, std::uint32_t kadIp, std::uint32_t publicIp)
{
	return keyIp != 0 ? keyIp : (kadIp != 0 ? kadIp : publicIp);
}
} // namespace NatSourceEcho

#endif // AMULE_NAT_SOURCE_ECHO_H
