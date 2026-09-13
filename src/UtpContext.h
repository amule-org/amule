//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//
// Any parts of this program derived from the xMule, lMule or eMule project,
// or contributed by third-party developers are copyrighted by their
// respective authors.
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
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA
//

#ifndef UTP_CONTEXT_H
#define UTP_CONTEXT_H

#include "ReservedProtocolFrames.h"

#include <cstdint>
#include <map>
#include <memory>
#include <utility>

// IPv4 uses aMule's low-byte-first integer representation; ports are host order.
class IUtpDatagramSink
{
public:
	virtual ~IUtpDatagramSink() = default;
	/**
	 * Sends one uTP datagram, with the crypt parameters of the socket it came
	 * from rather than any derived from the destination address.
	 *
	 * @a userHash is borrowed for the duration of the call and is null exactly
	 * when @a encrypt is false.
	 */
	virtual void SendUtpDatagram(const uint8_t *payload,
		size_t length,
		uint32_t ip,
		uint16_t port,
		bool encrypt,
		const uint8_t *userHash) = 0;
};

class IUtpContext
{
public:
	virtual ~IUtpContext() = default;
	virtual bool Configure() = 0;
	virtual void Destroy() = 0;
	virtual bool ProcessDatagram(const uint8_t *payload, size_t length, uint32_t ip, uint16_t port) = 0;
	virtual void Tick() = 0;

	//! True while that endpoint holds at least one accepted uTP socket.
	virtual bool HasRegisteredPeer(uint32_t ip, uint16_t port) const = 0;
};

/**
 * Which endpoints hold a uTP socket.
 *
 * Counted, not a set: one endpoint can hold several sockets, and forgetting on
 * the first close would leave the survivor's traffic answered with an RST.
 */
class CUtpPeerRegistry
{
public:
	void Add(uint32_t ip, uint16_t port) { ++m_peers[Key(ip, port)]; }

	void Remove(uint32_t ip, uint16_t port)
	{
		const auto found = m_peers.find(Key(ip, port));
		if (found == m_peers.end()) {
			return;
		}
		if (--found->second == 0) {
			m_peers.erase(found);
		}
	}

	bool Has(uint32_t ip, uint16_t port) const { return m_peers.find(Key(ip, port)) != m_peers.end(); }

	size_t Size() const { return m_peers.size(); }

private:
	static std::uint64_t Key(uint32_t ip, uint16_t port)
	{
		return (static_cast<std::uint64_t>(ip) << 16) | port;
	}

	std::map<std::uint64_t, unsigned> m_peers;
};

// Library seam: no libutp types or stream operations escape the adapter.
class IStreamTransport;

//! Where an accepted uTP stream is offered for admission.
class IUtpStreamAcceptor
{
public:
	virtual ~IUtpStreamAcceptor() = default;

	/**
	 * Offers one accepted stream.
	 *
	 * @return true when it takes ownership. A refusal leaves the transport to
	 * the adapter, which closes the socket only after libutp has finished
	 * processing the datagram that produced it.
	 *
	 * Taken by reference on purpose. A by-value parameter is destroyed by a
	 * refusal, and destroying a transport closes its libutp socket -- which
	 * the adapter would then close again. Ownership moves only on acceptance.
	 */
	virtual bool AcceptStream(
		std::unique_ptr<IStreamTransport> &transport, uint32_t ip, uint16_t port) = 0;
};

class IUtpLibrary
{
public:
	virtual ~IUtpLibrary() = default;
	virtual bool Create(IUtpDatagramSink &sink) = 0;
	virtual void Destroy() = 0;
	virtual bool ProcessDatagram(const uint8_t *payload, size_t length, uint32_t ip, uint16_t port) = 0;
	virtual void IssueDeferredAcks() = 0;
	virtual void CheckTimeouts() = 0;

	//! Null refuses every inbound SYN, which is the state before an acceptor exists.
	virtual void SetAcceptor(IUtpStreamAcceptor *acceptor) = 0;

	//! True while that endpoint holds at least one accepted socket.
	virtual bool HasRegisteredPeer(uint32_t ip, uint16_t port) const = 0;
};

// Main-thread only. Tick never recreates state abandoned by socket Close().
class CUtpContext final : public IUtpContext
{
public:
	CUtpContext(std::unique_ptr<IUtpLibrary> library, IUtpDatagramSink &sink)
	: m_library(std::move(library))
	, m_sink(sink)
	{
	}
	~CUtpContext() override { Destroy(); }
	bool Configure() override
	{
		if (!m_active) {
			m_active = m_library->Create(m_sink);
		}
		return m_active;
	}
	void Destroy() override
	{
		if (m_active) {
			m_library->Destroy();
			m_active = false;
		}
	}
	bool ProcessDatagram(const uint8_t *payload, size_t length, uint32_t ip, uint16_t port) override
	{
		if (!Configure()) {
			return false;
		}
		const bool claimed = m_library->ProcessDatagram(payload, length, ip, port);
		m_library->IssueDeferredAcks();
		return claimed;
	}
	void Tick() override
	{
		if (m_active) {
			m_library->IssueDeferredAcks();
			m_library->CheckTimeouts();
		}
	}

	// Delegated rather than answered here: registration happens where sockets
	// are created and destroyed, which is the library adapter.
	bool HasRegisteredPeer(uint32_t ip, uint16_t port) const override
	{
		return m_library->HasRegisteredPeer(ip, port);
	}

	//! Installed once. Until then every inbound SYN is refused.
	void SetAcceptor(IUtpStreamAcceptor *acceptor) { m_library->SetAcceptor(acceptor); }

private:
	std::unique_ptr<IUtpLibrary> m_library;
	IUtpDatagramSink &m_sink;
	bool m_active = false;
};

/**
 * What a uTP frame is, before libutp is allowed to answer it.
 *
 * libutp answers an unmatched non-SYN with an unsolicited RST to the claimed
 * source, which makes this host a reflector for a forged address.
 */
enum class EUtpFrameKind
{
	//! Shorter than a header, or a version libutp does not implement.
	Malformed,
	//! A connection request. Admission decides; no prior registration needed.
	Syn,
	//! Traffic for a connection, which only an already-registered peer can have.
	Existing
};

/**
 * Classifies one uTP frame from its header alone.
 *
 * Mirrors libutp's own validity test (UTP_Version, utp_internal.cpp): type
 * below ST_NUM_STATES, first extension below 3, version 1.
 */
inline EUtpFrameKind ClassifyUtpFrame(const uint8_t *payload, size_t length)
{
	// Header is 20 bytes in version 1; anything shorter cannot be parsed.
	constexpr size_t kUtpHeaderBytes = 20;
	constexpr uint8_t kUtpVersion = 1;
	constexpr uint8_t kStNumStates = 5;
	constexpr uint8_t kStSyn = 4;
	if (payload == nullptr || length < kUtpHeaderBytes) {
		return EUtpFrameKind::Malformed;
	}
	const uint8_t type = static_cast<uint8_t>(payload[0] >> 4);
	const uint8_t version = static_cast<uint8_t>(payload[0] & 0x0F);
	const uint8_t extension = payload[1];
	if (type >= kStNumStates || extension >= 3 || version != kUtpVersion) {
		return EUtpFrameKind::Malformed;
	}
	return type == kStSyn ? EUtpFrameKind::Syn : EUtpFrameKind::Existing;
}

inline bool ProcessUtpFrame(
	IUtpContext &context, const SReservedProt2Frame &frame, uint32_t ip, uint16_t port)
{
	return frame.disposition == RP2_KNOWN_TYPE && frame.type == OP_NATT_FRAME_UTP &&
	       context.ProcessDatagram(frame.payload, frame.payloadLength, ip, port);
}

// UDP sizing for libutp, accounting for the two-byte aMule envelope.
//
// libutp's own defaults (utp_default_get_udp_mtu / _overhead in utp_utils.cpp) branch on the
// address family and know nothing of the envelope, so overriding them is what makes libutp size
// packets that do not fragment. Branching the same way keeps the family awareness those defaults
// had: an IPv6 peer costs 20 more header bytes than IPv4, and libutp assumes Teredo because it
// cannot know the local interface either.
//
// Taken as a bool rather than a sockaddr so this stays free of socket headers and testable without
// one; the adapter does the sa_family comparison.
constexpr std::uint64_t kUtpEnvelopeBytes = 2;

// What CEncryptedDatagramSocket::EncryptSendClient() prepends. Its cryptHeaderLen is
// `padLen + CRYPT_HEADER_WITHOUTPADDING + (kad ? 8 : 0)`, which is 8 here because a uTP datagram
// is not Kad and padLen is hardcoded to zero -- under a comment reading "padding disabled for UDP
// currently". Enabling it makes this budget too small again, which is the defect this constant
// exists to fix, so that switch has to come back here.
//
// Subtracted unconditionally rather than only for peers we encrypt to, because the budget is a
// property of the libutp context while the decision is per-datagram, and a budget that is right
// only sometimes is the fragmentation this override exists to prevent. The cost of being wrong
// in this direction is eight bytes of payload on a plaintext datagram.
constexpr std::uint64_t kUtpCryptHeaderBytes = 8;

constexpr std::uint64_t UtpUdpMtu(bool isIPv6)
{
	// IPv4:   1500 ethernet - 20 IPv4 - 8 UDP - 24 GRE - 8 PPPoE - 2 MPPE - 36 fudge.
	// Teredo: 1280 - 40 IPv6 - 8 UDP.
	return (isIPv6 ? UINT64_C(1232) : UINT64_C(1402)) - kUtpEnvelopeBytes - kUtpCryptHeaderBytes;
}

constexpr std::uint64_t UtpUdpOverhead(bool isIPv6)
{
	// IPv4: 20 + 8. Teredo: that, plus 40 IPv6 + 8 UDP again.
	return (isIPv6 ? UINT64_C(76) : UINT64_C(28)) + kUtpEnvelopeBytes;
}

// Keep CPacket's application types out of the libutp translation unit.
template <typename Packet, typename Socket>
void QueueUtpDatagram(Socket &socket,
	const uint8_t *payload,
	size_t length,
	uint32_t ip,
	uint16_t port,
	bool encrypt,
	const uint8_t *hash)
{
	// Maximum IPv4 UDP payload, less the aMule envelope.
	if (length > 65507 - 2 || (length != 0 && payload == nullptr)) {
		return;
	}
	auto packet = std::make_unique<Packet>(
		OP_NATT_FRAME_UTP, static_cast<uint32_t>(length), OP_UDPRESERVEDPROT2);
	if (length != 0) {
		packet->CopyToDataBuffer(0, payload, static_cast<unsigned int>(length));
	}
	socket.SendPacket(packet.release(), ip, port, encrypt, hash, false, 0);
}

#endif // UTP_CONTEXT_H
// File_checked_for_headers
