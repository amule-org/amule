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
#include "UtpDialPolicy.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
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
	//! Sends our key frame, obfuscated with @a peerHash, ahead of an obfuscated dial's SYN.
	virtual void SendUtpKeyFrame(uint32_t, uint16_t, const uint8_t *) {}
	//! A connected stream now follows its peer to a new source port, after libutp took the frame.
	virtual void OnUtpStreamMoved() {}
};

class IStreamTransport;

struct SUtpDialOptions
{
	bool allowInitialPortRemap = false;
	std::uint64_t nowMs = 0;
	uint32_t clientToken = 0; // Local owner token only; OP_HELLO still establishes peer identity.
};

class IUtpContext
{
public:
	virtual ~IUtpContext() = default;
	virtual bool Configure() = 0;
	virtual bool IsAvailable() const = 0;
	virtual void Destroy() = 0;
	virtual bool ProcessDatagram(const uint8_t *payload, size_t length, uint32_t ip, uint16_t port) = 0;
	virtual bool ProcessDatagram(
		const uint8_t *payload, size_t length, uint32_t ip, uint16_t port, std::uint64_t nowMs)
	{
		return ProcessDatagram(payload, length, ip, port);
	}
	//! @p obfuscated is whether the datagram arrived under the UDP obfuscation layer.
	virtual bool ProcessDatagram(const uint8_t *payload,
		size_t length,
		uint32_t ip,
		uint16_t port,
		std::uint64_t nowMs,
		[[maybe_unused]] bool obfuscated)
	{
		return ProcessDatagram(payload, length, ip, port, nowMs);
	}
	//! @p preHelloKey, when set, obfuscates the reply to a SYN in this datagram until the hello.
	virtual bool ProcessDatagram(const uint8_t *payload,
		size_t length,
		uint32_t ip,
		uint16_t port,
		std::uint64_t nowMs,
		bool obfuscated,
		[[maybe_unused]] const uint8_t *preHelloKey)
	{
		return ProcessDatagram(payload, length, ip, port, nowMs, obfuscated);
	}
	//! Keys a plain dial to that endpoint, still in its handshake, with a hash its key frame announced.
	virtual bool ApplyAnnouncedKey(uint32_t, uint16_t, const uint8_t *) { return false; }
	virtual void Tick() = 0;

	// Unactivated prerequisite. Success transfers a pending stream into an
	// initially empty output; it is NOT an application connection notification.
	// False leaves output untouched, allowing the caller's legacy fallback.
	// The caller must first apply DecideUtpDial and existing security checks.
	// Polling transport state is possible, but attaching it to a client awaits
	// explicit connection-completion plumbing (never a writability event).
	// Default refusal keeps contexts without outbound support fail-closed.
	virtual bool Dial(uint32_t, uint16_t, bool, const uint8_t *, std::unique_ptr<IStreamTransport> &)
	{
		return false;
	}
	virtual bool Dial(uint32_t ip,
		uint16_t port,
		bool encrypt,
		const uint8_t *userHash,
		const SUtpDialOptions &,
		std::unique_ptr<IStreamTransport> &transport)
	{
		return Dial(ip, port, encrypt, userHash, transport);
	}

	//! True while that endpoint holds at least one accepted or outbound socket.
	virtual bool HasRegisteredPeer(uint32_t ip, uint16_t port) const = 0;
	//! True only for one live, remap-enabled handshake peer near the observed source port.
	virtual bool HasExpectedPortRemapPeer(uint32_t, uint16_t, std::uint64_t) const { return false; }
	//! True only when one connected stream on that address could own this source port, as a NAT would
	//! have moved it. Read-only: it neither installs a translation nor spends the move budget.
	virtual bool HasRepointCandidate(uint32_t, uint16_t) const { return false; }
};

// Counted, not a set: one endpoint can hold several sockets, and forgetting on
// the first close would leave the survivor answered with an RST.
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

// Library seam: no libutp types escape the adapter.

struct SUtpSynCandidateInfo
{
	EUtpSynCandidateStatus status = EUtpSynCandidateStatus::None;
	uint32_t ip = 0;
	uint16_t port = 0;
	uint32_t clientToken = 0;
};

enum class EUtpIncomingSynDisposition
{
	Accept,
	RejectKeepOutgoing,
	AcceptKeepIncoming
};

struct SUtpIncomingSynDecision
{
	EUtpIncomingSynDisposition disposition = EUtpIncomingSynDisposition::Accept;
	uint32_t candidateToken = 0;
};

//! Where an accepted uTP stream is offered for admission.
class IUtpStreamAcceptor
{
public:
	virtual ~IUtpStreamAcceptor() = default;

	/**
	 * Whether a SYN may open a stream at all, asked before libutp creates a socket or sends the
	 * SYN-ACK, so a refusal sends nothing back to a source that may be forged. @a halfOpen counts the
	 * inbound streams still waiting for their peer's first data.
	 */
	virtual bool AdmitsSyn(uint32_t, uint16_t, std::size_t) { return true; }

	//! Selects the simultaneous-open disposition before a transport is transferred.
	virtual SUtpIncomingSynDecision DecideIncomingSyn(uint32_t, uint16_t, const SUtpSynCandidateInfo &)
	{
		return {};
	}

	/**
	 * Offers one accepted stream.
	 *
	 * By reference on purpose: a by-value parameter is destroyed by a refusal,
	 * and that closes the socket the adapter then closes again.
	 */
	virtual bool AcceptStream(
		std::unique_ptr<IStreamTransport> &transport, uint32_t ip, uint16_t port) = 0;
	virtual bool AcceptStream(std::unique_ptr<IStreamTransport> &transport,
		uint32_t ip,
		uint16_t port,
		const SUtpIncomingSynDecision &)
	{
		return AcceptStream(transport, ip, port);
	}
};

class IUtpLibrary
{
public:
	virtual ~IUtpLibrary() = default;
	virtual bool Create(IUtpDatagramSink &sink) = 0;
	virtual void Destroy() = 0;
	virtual bool ProcessDatagram(const uint8_t *payload, size_t length, uint32_t ip, uint16_t port) = 0;
	virtual bool ProcessDatagram(
		const uint8_t *payload, size_t length, uint32_t ip, uint16_t port, std::uint64_t)
	{
		return ProcessDatagram(payload, length, ip, port);
	}
	//! @p obfuscated is carried to the stream a SYN in this datagram opens.
	virtual bool ProcessDatagram(const uint8_t *payload,
		size_t length,
		uint32_t ip,
		uint16_t port,
		std::uint64_t nowMs,
		[[maybe_unused]] bool obfuscated)
	{
		return ProcessDatagram(payload, length, ip, port, nowMs);
	}
	//! @p preHelloKey, when set, obfuscates the reply to a SYN in this datagram until the hello.
	virtual bool ProcessDatagram(const uint8_t *payload,
		size_t length,
		uint32_t ip,
		uint16_t port,
		std::uint64_t nowMs,
		bool obfuscated,
		[[maybe_unused]] const uint8_t *preHelloKey)
	{
		return ProcessDatagram(payload, length, ip, port, nowMs, obfuscated);
	}
	//! Keys a plain dial to that endpoint, still in its handshake, with a hash its key frame announced.
	virtual bool ApplyAnnouncedKey(uint32_t, uint16_t, const uint8_t *) { return false; }
	virtual void IssueDeferredAcks() = 0;
	virtual void CheckTimeouts() = 0;

	// IPv4/host-order port; crypt parameters are installed before the SYN.
	// Caller must retain the library until the returned transport is destroyed.
	virtual bool Dial(uint32_t, uint16_t, bool, const uint8_t *, std::unique_ptr<IStreamTransport> &)
	{
		return false;
	}
	virtual bool Dial(uint32_t ip,
		uint16_t port,
		bool encrypt,
		const uint8_t *userHash,
		const SUtpDialOptions &,
		std::unique_ptr<IStreamTransport> &transport)
	{
		return Dial(ip, port, encrypt, userHash, transport);
	}

	//! Null refuses every inbound SYN, which is the state before an acceptor exists.
	virtual void SetAcceptor(IUtpStreamAcceptor *acceptor) = 0;

	//! True while that endpoint holds at least one accepted or outbound socket.
	virtual bool HasRegisteredPeer(uint32_t ip, uint16_t port) const = 0;
	//! True only for one live, remap-enabled handshake peer near the observed source port.
	virtual bool HasExpectedPortRemapPeer(uint32_t, uint16_t, std::uint64_t) const { return false; }
	//! True only when one connected stream on that address could own this source port, as a NAT would
	//! have moved it. Read-only: it neither installs a translation nor spends the move budget.
	virtual bool HasRepointCandidate(uint32_t, uint16_t) const { return false; }
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
			// Re-applied on every rebuild, not once at construction. Destroy()
			// drops the library's acceptor, and CClientUDPSocket::Close() is
			// followed by Open() on a Kad reconnect without the object being
			// rebuilt -- so an acceptor installed only in the constructor is
			// gone for the session and every SYN is refused silently.
			if (m_active) {
				m_library->SetAcceptor(m_acceptor);
			}
		}
		return m_active;
	}
	bool IsAvailable() const override { return m_active; }
	void Destroy() override
	{
		if (m_active) {
			m_library->Destroy();
			m_active = false;
		}
	}
	bool ProcessDatagram(const uint8_t *payload, size_t length, uint32_t ip, uint16_t port) override
	{
		return ProcessDatagram(payload, length, ip, port, 0);
	}
	bool ProcessDatagram(const uint8_t *payload,
		size_t length,
		uint32_t ip,
		uint16_t port,
		std::uint64_t nowMs) override
	{
		return ProcessDatagram(payload, length, ip, port, nowMs, false);
	}
	bool ProcessDatagram(const uint8_t *payload,
		size_t length,
		uint32_t ip,
		uint16_t port,
		std::uint64_t nowMs,
		bool obfuscated) override
	{
		return ProcessDatagram(payload, length, ip, port, nowMs, obfuscated, nullptr);
	}
	bool ProcessDatagram(const uint8_t *payload,
		size_t length,
		uint32_t ip,
		uint16_t port,
		std::uint64_t nowMs,
		bool obfuscated,
		const uint8_t *preHelloKey) override
	{
		if (!Configure()) {
			return false;
		}
		const bool claimed =
			m_library->ProcessDatagram(payload, length, ip, port, nowMs, obfuscated, preHelloKey);
		m_library->IssueDeferredAcks();
		return claimed;
	}
	bool ApplyAnnouncedKey(uint32_t ip, uint16_t port, const uint8_t *userHash) override
	{
		return m_active && m_library->ApplyAnnouncedKey(ip, port, userHash);
	}
	void Tick() override
	{
		if (m_active) {
			m_library->IssueDeferredAcks();
			m_library->CheckTimeouts();
		}
	}

	bool Dial(uint32_t ip,
		uint16_t port,
		bool encrypt,
		const uint8_t *userHash,
		std::unique_ptr<IStreamTransport> &transport) override
	{
		return Dial(ip, port, encrypt, userHash, SUtpDialOptions{}, transport);
	}
	bool Dial(uint32_t ip,
		uint16_t port,
		bool encrypt,
		const uint8_t *userHash,
		const SUtpDialOptions &options,
		std::unique_ptr<IStreamTransport> &transport) override
	{
		// Never reopen a closed UDP service as a side effect of dialing.
		if (!m_active || transport || ip == 0 || port == 0 || (encrypt && userHash == nullptr)) {
			return false;
		}
		return m_library->Dial(ip, port, encrypt, userHash, options, transport);
	}

	// Delegated rather than answered here: registration happens where sockets
	// are created and destroyed, which is the library adapter.
	bool HasRegisteredPeer(uint32_t ip, uint16_t port) const override
	{
		return m_library->HasRegisteredPeer(ip, port);
	}
	bool HasExpectedPortRemapPeer(uint32_t ip, uint16_t port, std::uint64_t nowMs) const override
	{
		return m_library->HasExpectedPortRemapPeer(ip, port, nowMs);
	}
	bool HasRepointCandidate(uint32_t ip, uint16_t port) const override
	{
		return m_library->HasRepointCandidate(ip, port);
	}

	//! Held, so it survives the context being destroyed and rebuilt.
	void SetAcceptor(IUtpStreamAcceptor *acceptor)
	{
		m_acceptor = acceptor;
		if (m_active) {
			m_library->SetAcceptor(acceptor);
		}
	}

private:
	std::unique_ptr<IUtpLibrary> m_library;
	IUtpDatagramSink &m_sink;
	IUtpStreamAcceptor *m_acceptor = nullptr;
	bool m_active = false;
};

//! libutp answers an unmatched non-SYN with an unsolicited RST to the claimed
//! source, which makes this host a reflector for a forged address.
enum class EUtpFrameKind
{
	//! Shorter than a header, or a version libutp does not implement.
	Malformed,
	//! A connection request. Admission decides; no prior registration needed.
	Syn,
	//! Traffic for a connection, which only an already-registered peer can have.
	Existing
};

//! Header is 20 bytes in version 1 (PacketFormatV1, utp_internal.cpp:113-136); anything shorter
//! cannot be parsed.
constexpr size_t kUtpWireHeaderBytes = 20;
//! libutp's validity test constants, shared with ClassifyUtpFrame().
constexpr uint8_t kUtpVersion = 1;
constexpr uint8_t kStNumStates = 5;

//! The largest UDP payload on a 1500-byte IPv4 link. No uTP stack sends a frame above its MTU, and
//! libutp would hold a larger one in its 1023-slot reorder buffer all the same.
constexpr size_t kMaxUtpFrameBytes = 1472;

//! Mirrors libutp's validity test (UTP_Version): type < ST_NUM_STATES,
//! extension < 3, version 1. A frame above kMaxUtpFrameBytes is malformed too.
inline EUtpFrameKind ClassifyUtpFrame(const uint8_t *payload, size_t length)
{
	if (payload == nullptr || length < kUtpWireHeaderBytes || length > kMaxUtpFrameBytes) {
		return EUtpFrameKind::Malformed;
	}
	const uint8_t type = static_cast<uint8_t>(payload[0] >> 4);
	const uint8_t version = static_cast<uint8_t>(payload[0] & 0x0F);
	const uint8_t extension = payload[1];
	if (type >= kStNumStates || extension >= 3 || version != kUtpVersion) {
		return EUtpFrameKind::Malformed;
	}
	return type == kUtpTypeSyn ? EUtpFrameKind::Syn : EUtpFrameKind::Existing;
}

//! Reads the base header of a frame ClassifyUtpFrame() accepts. Returns nothing for anything it does
//! not: no decision may rest on a frame libutp itself would refuse to parse. Field offsets are
//! PacketFormatV1's, all big endian (utp_internal.cpp:113-136): ver_type, ext, connid at 2, tv_usec,
//! reply_micro, windowsize, seq_nr at 16, ack_nr at 18. Extensions follow the base header.
inline std::optional<SUtpMoveHeader> ReadUtpWireHeader(const uint8_t *payload, size_t length)
{
	const EUtpFrameKind kind = ClassifyUtpFrame(payload, length);
	if (kind == EUtpFrameKind::Malformed) {
		return std::nullopt;
	}
	const auto bigEndian16 = [payload](size_t offset) {
		return static_cast<std::uint16_t>(
			(static_cast<std::uint16_t>(payload[offset]) << 8) | payload[offset + 1]);
	};
	SUtpMoveHeader header;
	header.type = static_cast<std::uint8_t>(payload[0] >> 4);
	header.connectionId = bigEndian16(2);
	header.sequenceNumber = bigEndian16(16);
	header.acknowledgeNumber = bigEndian16(18);
	return header;
}

inline bool ProcessUtpFrame(
	IUtpContext &context, const SReservedProt2Frame &frame, uint32_t ip, uint16_t port)
{
	return frame.disposition == RP2_KNOWN_TYPE && frame.type == OP_NATT_FRAME_UTP &&
	       context.ProcessDatagram(frame.payload, frame.payloadLength, ip, port);
}

inline bool ProcessUtpFrame(IUtpContext &context,
	const SReservedProt2Frame &frame,
	uint32_t ip,
	uint16_t port,
	std::uint64_t nowMs,
	bool obfuscated,
	const uint8_t *preHelloKey = nullptr)
{
	return frame.disposition == RP2_KNOWN_TYPE && frame.type == OP_NATT_FRAME_UTP &&
	       context.ProcessDatagram(
		       frame.payload, frame.payloadLength, ip, port, nowMs, obfuscated, preHelloKey);
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
	socket.SendTransportPacket(packet.release(), ip, port, encrypt, hash);
}

#endif // UTP_CONTEXT_H
// File_checked_for_headers
