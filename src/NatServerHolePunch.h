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
// Experimental server-coordinated NAT-T payload codecs. No socket integration.
#ifndef AMULE_NAT_SERVER_HOLE_PUNCH_H
#define AMULE_NAT_SERVER_HOLE_PUNCH_H

#ifdef ENABLE_NATT_SERVER_COORDINATION
#include "ArchSpecific.h"
#include "NetworkFunctions.h" // Needed for IsLowID
#include "include/protocol/Protocols.h"
#include "include/protocol/ed2k/Client2Server/TCP.h" // Needed for SRV_TCPFLG_LOWID_HOLEPUNCH
#include "include/protocol/ed2k/Client2Server/UDP.h" // Needed for SRV_UDPFLG_FOREIGN_NAT_CALLBACK

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

namespace NatServerHolePunch
{
using Hash = std::array<std::uint8_t, 16>;
using IPv4 = std::array<std::uint8_t, 4>;

struct Request
{
	std::uint32_t targetId;
	std::uint16_t udpPort;
};
struct Info
{
	// Octets in wire order, e.g. {192, 0, 2, 1}, not a host-order integer.
	IPv4 peerIP;
	std::uint16_t tcpPort;
	std::uint16_t udpPort;
	Hash userHash;
	std::uint8_t role; // Opaque: decoding does not authorize a peer action.
	// The server ID of the peer described, present only in a frame of 29 bytes or more.
	std::optional<std::uint32_t> peerId;
	// The peer's crypt-layer options, (CT_SERVER_FLAGS >> 9) & 7, only in a frame of 30 bytes or more.
	std::optional<std::uint8_t> peerCrypt;
};
struct Failure
{
	std::uint32_t targetId;
	std::uint8_t reason; // Preserve unknown reasons; never treat one as success.
};

// Send OP_LOWID_HOLEPUNCH_REQUEST only to a server whose OP_IDCHANGE flags announce it.
inline bool ServerAcceptsHolePunchRequest(std::uint32_t tcpFlags)
{
	return (tcpFlags & SRV_TCPFLG_LOWID_HOLEPUNCH) != 0;
}

inline bool IsServerTcpOpcode(std::uint8_t opcode)
{
	return opcode == NatServerTcp::OP_LOWID_HOLEPUNCH_REQUEST ||
	       opcode == NatServerTcp::OP_LOWID_HOLEPUNCH_INFO ||
	       opcode == NatServerTcp::OP_LOWID_HOLEPUNCH_FAIL;
}
inline bool IsServerUdpOpcode(std::uint8_t opcode)
{
	return opcode == NatServerUdp::OP_NATT_KEEPALIVE;
}
inline bool IsPeerUdpOpcode(std::uint8_t opcode)
{
	return opcode == NatPeerUdp::OP_NATT_HOLEPUNCH;
}

namespace Detail
{
inline void WriteLE(std::uint8_t *out, std::uint32_t value, std::size_t count)
{
	if (count == 2) {
		PokeUInt16(out, static_cast<uint16>(value));
	} else {
		PokeUInt32(out, static_cast<uint32>(value));
	}
}
inline std::uint32_t ReadLE(const std::uint8_t *data, std::size_t count)
{
	return count == 2 ? PeekUInt16(data) : PeekUInt32(data);
}
} // namespace Detail

// Payloads only: callers must supply transport/protocol/opcode framing separately.
inline std::array<std::uint8_t, 6> BuildRequest(const Request &request)
{
	std::array<std::uint8_t, 6> bytes{};
	Detail::WriteLE(bytes.data(), request.targetId, 4);
	Detail::WriteLE(bytes.data() + 4, request.udpPort, 2);
	return bytes;
}
inline std::optional<Request> ParseRequest(const std::uint8_t *data, std::size_t size)
{
	if (!data || size < 6) {
		return std::nullopt;
	}
	return Request{ Detail::ReadLE(data, 4), static_cast<std::uint16_t>(Detail::ReadLE(data + 4, 2)) };
}
inline std::array<std::uint8_t, 25> BuildInfo(const Info &info)
{
	std::array<std::uint8_t, 25> bytes{};
	std::copy(info.peerIP.begin(), info.peerIP.end(), bytes.begin());
	Detail::WriteLE(bytes.data() + 4, info.tcpPort, 2);
	Detail::WriteLE(bytes.data() + 6, info.udpPort, 2);
	std::copy(info.userHash.begin(), info.userHash.end(), bytes.begin() + 8);
	bytes[24] = info.role;
	return bytes;
}
inline std::optional<Info> ParseInfo(const std::uint8_t *data, std::size_t size)
{
	if (!data || size < 25) {
		return std::nullopt;
	}
	Info info{};
	std::copy_n(data, 4, info.peerIP.begin());
	info.tcpPort = static_cast<std::uint16_t>(Detail::ReadLE(data + 4, 2));
	info.udpPort = static_cast<std::uint16_t>(Detail::ReadLE(data + 6, 2));
	std::copy_n(data + 8, 16, info.userHash.begin());
	info.role = data[24];
	if (size >= 29) {
		info.peerId = Detail::ReadLE(data + 25, 4);
	}
	if (size >= 30) {
		info.peerCrypt = data[29];
	}
	return info;
}
inline std::array<std::uint8_t, 5> BuildFailure(const Failure &failure)
{
	std::array<std::uint8_t, 5> bytes{};
	Detail::WriteLE(bytes.data(), failure.targetId, 4);
	bytes[4] = failure.reason;
	return bytes;
}
inline std::optional<Failure> ParseFailure(const std::uint8_t *data, std::size_t size)
{
	if (!data || size < 5) {
		return std::nullopt;
	}
	return Failure{ Detail::ReadLE(data, 4), data[4] };
}
// Keepalive framing: protocol byte 0xE3, NAT-T payload opcode 0x9F, sent via
// the client's UDP socket to the server TCP port plus 4.
inline Hash BuildKeepalive(const Hash &ownUserHash)
{
	return ownUserHash;
}
inline std::optional<Hash> ParseKeepalive(const std::uint8_t *data, std::size_t size)
{
	if (!data || size < 16) {
		return std::nullopt;
	}
	Hash hash{};
	std::copy_n(data, 16, hash.begin());
	return hash;
}

// CT_EMULE_UDPPORTS (0xF9) login tag value: high 16 bits Kad UDP port, low 16 bits client UDP
// port. aMule shares one UDP socket for both, so the two arguments are usually equal.
inline std::uint32_t BuildUdpPortsLoginValue(std::uint16_t kadUdpPort, std::uint16_t clientUdpPort)
{
	return (static_cast<std::uint32_t>(kadUdpPort) << 16) | clientUdpPort;
}

struct KeepaliveFacts
{
	bool settingEnabled = false;
	bool connected = false;
	bool serverAnnouncesHolePunch = false;
	bool weAreLowId = false;
	std::uint16_t udpPort = 0;
	// Without uTP, advertising support would enrol a client that cannot act on a punch.
	bool utpAvailable = false;
};

// Whether to send OP_SERVER_NATT_KEEPALIVE now: connected to a server that announced
// SRV_TCPFLG_LOWID_HOLEPUNCH, with the runtime setting on, while we are LowID with UDP enabled
// and the build can act on it.
inline bool ShouldSendKeepalive(const KeepaliveFacts &facts)
{
	return facts.settingEnabled && facts.connected && facts.serverAnnouncesHolePunch &&
	       facts.weAreLowId && facts.udpPort != 0 && facts.utpAvailable;
}

// Do not enrol a build that cannot serve the uTP connection after the punch.
inline bool ShouldAdvertiseUdpPorts(bool settingEnabled, bool utpAvailable) noexcept
{
	return settingEnabled && utpAvailable;
}

struct RequestFacts
{
	std::uint32_t ourId = 0;
	std::uint32_t sourceId = 0;
	bool sameServer = false;
	bool serverAnnouncesHolePunch = false;
	bool settingEnabled = false;
	std::uint16_t ourUdpPort = 0;
	// See KeepaliveFacts::utpAvailable.
	bool utpAvailable = false;
};

// Whether to send OP_LOWID_HOLEPUNCH_REQUEST for a LowID source: both ends must be LowID on the
// same, hole-punch-capable, non-Lugdunum server, the runtime setting must be on, our UDP port
// must be enabled, and the build must be able to act on the server's word.
inline bool ShouldRequestLowIdHolePunch(const RequestFacts &facts)
{
	if (!facts.settingEnabled || !facts.sameServer || !facts.serverAnnouncesHolePunch ||
		facts.ourUdpPort == 0 || !facts.utpAvailable) {
		return false;
	}
	return IsLowID(facts.ourId) && IsLowID(facts.sourceId);
}

struct LowIdServerSourceFacts
{
	bool originIsLocalServer = false;
	bool sourceServerIsOurCurrentServer = false;
	bool weAreLowId = false;
	bool serverAnnouncesHolePunch = false;
	bool settingEnabled = false;
	// We announce the hole-punch extension (ShouldAnnounceHolePunchExtension()).
	bool extensionAnnounced = false;
	// The source's server is not ours but takes a cross-server request (ServerTakesForeignCallback()).
	bool sourceServerTakesForeignCallback = false;
};

// What we know of a source's server, to ask it for a cross-server NAT callback (ed2k-server 0.9.86).
struct ForeignServerFacts
{
	bool inServerList = false;
	std::uint32_t udpFlags = 0;
	std::uint32_t serverKeyUdp = 0;
	bool supportsObfuscationUdp = false;
	bool cryptLayerUdpEnabled = false;
};

// A source's server address is learnt from a peer, so only a server in our own list is asked. The
// request is server-key obfuscated, so it also needs that server's UDP key for our address.
inline bool ServerTakesForeignCallback(const ForeignServerFacts &facts)
{
	return facts.inServerList && (facts.udpFlags & SRV_UDPFLG_FOREIGN_NAT_CALLBACK) != 0 &&
	       facts.serverKeyUdp != 0 && facts.supportsObfuscationUdp && facts.cryptLayerUdpEnabled;
}

// Whether a LowID source survives CanAddSource's LowID-while-firewalled drop. Since ed2k-server
// 0.9.86 (ed2k-server#49) a LowID requester with NAT traversal gets only LowID sources that have it
// too, and aMule qualifies through the UDP port it sends at login while LowIdHolePunch is on. Kept
// are sources from our own hole-punch-capable server, and sources of another server that takes a
// cross-server request, which we can ask while we announce the extension.
inline bool ShouldKeepLowIdServerSource(const LowIdServerSourceFacts &facts)
{
	if (!facts.weAreLowId) {
		return false;
	}
	if (facts.sourceServerIsOurCurrentServer) {
		return facts.settingEnabled && facts.originIsLocalServer && facts.serverAnnouncesHolePunch;
	}
	return facts.extensionAnnounced && facts.sourceServerTakesForeignCallback;
}

// Server-coordinated punch state.

// A 0x61 names the peer's server ID, so every 0x60 still waiting for its answer can be matched
// exactly. A request that never gets one expires after 30 s.
constexpr std::uint64_t kPendingRequestTimeoutMs = 30'000;
constexpr std::size_t kMaxPendingRequests = 8;

class PendingRequestSet
{
public:
	bool CanSend(std::uint32_t targetId, std::uint64_t now)
	{
		Evict(now);
		return m_pending.size() < kMaxPendingRequests && Find(targetId) == m_pending.end();
	}
	void OnSent(std::uint32_t targetId, std::uint64_t now)
	{
		Evict(now);
		if (Find(targetId) == m_pending.end()) {
			m_pending.push_back({ targetId, now });
		}
	}
	// True when a request for targetId was pending, which makes this side its requester.
	bool Take(std::uint32_t targetId, std::uint64_t now)
	{
		Evict(now);
		const auto match = Find(targetId);
		if (match == m_pending.end()) {
			return false;
		}
		m_pending.erase(match);
		return true;
	}
	// A 0x62 identifies the failed target.
	void Release(std::uint32_t targetId) { Take(targetId, 0); }

private:
	struct Entry
	{
		std::uint32_t targetId;
		std::uint64_t sentAt;
	};
	std::vector<Entry>::iterator Find(std::uint32_t targetId)
	{
		return std::find_if(m_pending.begin(), m_pending.end(), [targetId](const Entry &entry) {
			return entry.targetId == targetId;
		});
	}
	void Evict(std::uint64_t now)
	{
		const auto firstLive =
			std::remove_if(m_pending.begin(), m_pending.end(), [now](const Entry &entry) {
				return now >= entry.sentAt && now - entry.sentAt >= kPendingRequestTimeoutMs;
			});
		m_pending.erase(firstLive, m_pending.end());
	}
	std::vector<Entry> m_pending;
};

// ed2k-server resends 0x61 on purpose, up to three times per side. 10 s spans
// that burst without silently dropping a later, independent rendezvous for the same peer.
constexpr std::uint64_t kDuplicateInfoWindowMs = 10'000;
// Mirrors CRequesterLimiter's cap (NatRendezvousPolicy.h): bounds memory from a server that
// never repeats, not just a well-behaved one.
constexpr std::size_t kMaxTrackedDuplicates = 256;

// Suppresses a repeated 0x61 for the same peer: keyed by user hash, which every 0x61 carries
// regardless of role, unlike the target ID which only the requester side always has.
class DuplicateInfoSuppressor
{
public:
	// True the first time @p peerHash is seen inside the window; false for a repeat.
	bool ShouldProcess(const Hash &peerHash, std::uint64_t now)
	{
		Evict(now);
		for (const Entry &entry : m_seen) {
			if (entry.peerHash == peerHash) {
				return false;
			}
		}
		if (m_seen.size() >= kMaxTrackedDuplicates) {
			m_seen.erase(m_seen.begin());
		}
		m_seen.push_back({ peerHash, now });
		return true;
	}

private:
	struct Entry
	{
		Hash peerHash;
		std::uint64_t seenAt;
	};
	void Evict(std::uint64_t now)
	{
		const auto firstLive =
			std::remove_if(m_seen.begin(), m_seen.end(), [now](const Entry &entry) {
				return now >= entry.seenAt && now - entry.seenAt >= kDuplicateInfoWindowMs;
			});
		m_seen.erase(firstLive, m_seen.end());
	}
	std::vector<Entry> m_seen;
};

// A server can point us at any IP; cap server-directed punches and dials. 20/minute
// accommodates normal multi-source use while bounding the reach of a hostile server.
constexpr std::uint64_t kActionWindowMs = 60'000;
constexpr std::size_t kMaxActionsPerWindow = 20;

// Shared by both dials (requester role) and punches (target role) triggered by 0x61: one global
// budget, since both are the same risk -- the server choosing our next action.
class ActionRateLimiter
{
public:
	bool Admit(std::uint64_t now)
	{
		Evict(now);
		if (m_events.size() >= kMaxActionsPerWindow) {
			return false;
		}
		m_events.push_back(now);
		return true;
	}

private:
	void Evict(std::uint64_t now)
	{
		while (!m_events.empty() && now >= m_events.front() &&
			now - m_events.front() >= kActionWindowMs) {
			m_events.pop_front();
		}
	}
	std::deque<std::uint64_t> m_events;
};

// What a 0x61 told us about the peer, so a dial to it can be obfuscated from its first frame.
struct VouchedPeer
{
	Hash userHash{};
	std::optional<std::uint8_t> cryptOptions;
};

// A punch reply is accepted for 30 s after the server gives us its peer endpoint.
constexpr std::uint64_t kExpectedEndpointTtlMs = 30'000;

// What a peer's punch asks of us.
struct PunchAction
{
	enum Kind
	{
		None,
		DialNow,
		DialLater
	};
	Kind kind = None;
	std::uint16_t tcpPort = 0;
	VouchedPeer peer;
};

// A server-announced endpoint may trigger one dial back when its UDP punch arrives. The TCP
// port is kept only for a temporary client; OP_HELLO establishes the peer's actual identity.
class ExpectedEndpointRegistry
{
public:
	void Register(
		const IPv4 &peerIP, std::uint16_t tcpPort, std::uint64_t now, const VouchedPeer &peer = {})
	{
		Evict(now);
		if (tcpPort == 0) {
			return;
		}
		m_expected.push_back({ peerIP, tcpPort, now, std::nullopt, peer });
	}
	// Records the UDP port of a dial already started for this endpoint, so the punch that
	// follows from that same port does not start a second one.
	void NoteDialled(const IPv4 &peerIP, std::uint16_t udpPort)
	{
		const auto match = Find(peerIP);
		if (match != m_expected.end()) {
			match->dialledUdpPort = udpPort;
		}
	}
	// A punch from an endpoint we dialled needs nothing more from that port and a dial now
	// from another; a peer we did not dial gets a dial back later, if no stream arrives.
	PunchAction Take(const IPv4 &sourceIP, std::uint16_t sourcePort, std::uint64_t now)
	{
		Evict(now);
		const auto match = Find(sourceIP);
		if (match == m_expected.end()) {
			return {};
		}
		const Entry entry = *match;
		m_expected.erase(match);
		if (!entry.dialledUdpPort.has_value()) {
			return { PunchAction::DialLater, entry.tcpPort, entry.peer };
		}
		if (*entry.dialledUdpPort == sourcePort) {
			return {};
		}
		return { PunchAction::DialNow, entry.tcpPort, entry.peer };
	}

private:
	struct Entry
	{
		IPv4 peerIP;
		std::uint16_t tcpPort;
		std::uint64_t registeredAt;
		std::optional<std::uint16_t> dialledUdpPort;
		VouchedPeer peer;
	};
	std::vector<Entry>::iterator Find(const IPv4 &peerIP)
	{
		return std::find_if(m_expected.begin(), m_expected.end(), [&peerIP](const Entry &entry) {
			return entry.peerIP == peerIP;
		});
	}
	void Evict(std::uint64_t now)
	{
		const auto firstLive =
			std::remove_if(m_expected.begin(), m_expected.end(), [now](const Entry &entry) {
				return now >= entry.registeredAt &&
				       now - entry.registeredAt >= kExpectedEndpointTtlMs;
			});
		m_expected.erase(firstLive, m_expected.end());
	}
	std::vector<Entry> m_expected;
};

// The side that did not request the punch dials back only when the peer's own stream has not
// arrived by then, which normally leaves a single connection.
constexpr std::uint64_t kDialBackDelayMs = 3'000;
constexpr std::size_t kMaxDialBacks = 16;

class DialBackSchedule
{
public:
	struct Entry
	{
		IPv4 peerIP;
		std::uint16_t tcpPort;
		std::uint16_t udpPort;
		std::uint64_t dueAt;
		VouchedPeer peer;
	};
	void Schedule(const IPv4 &peerIP,
		std::uint16_t tcpPort,
		std::uint16_t udpPort,
		std::uint64_t now,
		const VouchedPeer &peer = {})
	{
		const auto same = std::find_if(m_entries.begin(),
			m_entries.end(),
			[&peerIP](const Entry &entry) { return entry.peerIP == peerIP; });
		if (same != m_entries.end()) {
			m_entries.erase(same);
		} else if (m_entries.size() >= kMaxDialBacks) {
			m_entries.erase(m_entries.begin());
		}
		m_entries.push_back({ peerIP, tcpPort, udpPort, now + kDialBackDelayMs, peer });
	}
	std::vector<Entry> TakeDue(std::uint64_t now)
	{
		std::vector<Entry> due;
		const auto firstPending = std::stable_partition(m_entries.begin(),
			m_entries.end(),
			[now](const Entry &entry) { return now >= entry.dueAt; });
		due.assign(m_entries.begin(), firstPending);
		m_entries.erase(m_entries.begin(), firstPending);
		return due;
	}

private:
	std::vector<Entry> m_entries;
};
// Hole-punch extension (ed2k-server 0.9.85). Between two clients that both announced it, a 0x60
// wakes the target with Lugdunum's 0x37; the target's fresh 0x9F then releases a 30-byte 0x61.

inline bool ServerOffersHolePunchExtension(std::uint32_t tcpFlags)
{
	return (tcpFlags & SRV_TCPFLG_HOLEPUNCH_EXTENSION) != 0;
}

// The extension needs Lugdunum's opt-in too: its 0x1000 enrols us for 0x37 forwards, which only
// the Lugdunum path serves.
inline bool ShouldAnnounceHolePunchExtension(
	bool holePunchEnabled, bool lugdunumCallbackEnabled, bool utpAvailable)
{
	return holePunchEnabled && lugdunumCallbackEnabled && utpAvailable;
}

struct KeepaliveCryptFacts
{
	bool extensionActive = false;
	bool cryptLayerUdpEnabled = false;
	std::uint32_t serverKeyUdp = 0;
	bool serverSupportsObfuscationUdp = false;
};

// An extension server ignores a session's plain 0x9F once it has sent an obfuscated one, so every
// 0x9F is obfuscated once the key exists. An older server reads only plain ones.
inline bool ShouldObfuscateKeepalive(const KeepaliveCryptFacts &facts)
{
	return facts.extensionActive && facts.cryptLayerUdpEnabled && facts.serverKeyUdp != 0 &&
	       facts.serverSupportsObfuscationUdp;
}

// A woken target sends 0x9F at 0, 0.5 and 1 s: the server takes the first that arrives.
constexpr std::array<std::uint64_t, 3> kWakeKeepaliveOffsetsMs{ 0, 500, 1'000 };
constexpr std::size_t kMaxPendingBurstKeepalives = 3 * kWakeKeepaliveOffsetsMs.size();

class KeepaliveBurst
{
public:
	void Start(std::uint64_t now)
	{
		for (const std::uint64_t offset : kWakeKeepaliveOffsetsMs) {
			if (m_dueAt.size() >= kMaxPendingBurstKeepalives) {
				m_dueAt.erase(m_dueAt.begin());
			}
			m_dueAt.push_back(now + offset);
		}
	}
	// How many keepalives are due now; each is returned once.
	std::size_t TakeDue(std::uint64_t now)
	{
		const auto firstPending = std::stable_partition(
			m_dueAt.begin(), m_dueAt.end(), [now](std::uint64_t dueAt) { return now >= dueAt; });
		const auto due = static_cast<std::size_t>(firstPending - m_dueAt.begin());
		m_dueAt.erase(m_dueAt.begin(), firstPending);
		return due;
	}

private:
	std::vector<std::uint64_t> m_dueAt;
};

// A 0x37 on an extension server may also come from a Lugdunum 0xA6, which no 0x61 follows. The
// server sends 0x61 within 3 s of the wake (resends at +1.2 s and +3 s); 5 s covers that.
constexpr std::uint64_t kWakeInfoTimeoutMs = 5'000;
constexpr std::size_t kMaxPendingWakes = 16;

// Wakes waiting for the 0x61 that names their requester; an unanswered one is handed back.
// Requester ID 0 is a cross-server request: no 0x61 follows, and the target waits to be dialled.
template <typename Wake> class PendingWakeSet
{
public:
	void Add(std::uint32_t requesterId, Wake wake, std::uint64_t now)
	{
		if (requesterId == 0) {
			return;
		}
		Resolve(requesterId);
		if (m_pending.size() >= kMaxPendingWakes) {
			m_pending.pop_front();
		}
		m_pending.push_back({ requesterId, std::move(wake), now });
	}
	bool Resolve(std::uint32_t requesterId)
	{
		const auto match = std::find_if(m_pending.begin(),
			m_pending.end(),
			[requesterId](const Entry &entry) { return entry.requesterId == requesterId; });
		if (match == m_pending.end()) {
			return false;
		}
		m_pending.erase(match);
		return true;
	}
	std::vector<Wake> TakeExpired(std::uint64_t now)
	{
		const auto firstLive =
			std::stable_partition(m_pending.begin(), m_pending.end(), [now](const Entry &entry) {
				return now >= entry.wokenAt && now - entry.wokenAt >= kWakeInfoTimeoutMs;
			});
		std::vector<Wake> expired;
		for (auto it = m_pending.begin(); it != firstLive; ++it) {
			expired.push_back(std::move(it->wake));
		}
		m_pending.erase(m_pending.begin(), firstLive);
		return expired;
	}

private:
	struct Entry
	{
		std::uint32_t requesterId;
		Wake wake;
		std::uint64_t wokenAt;
	};
	std::deque<Entry> m_pending;
};
// Statistics: which server-coordinated path a rendezvous took. The extension counts as the
// ed2k-server punch, and its Lugdunum fallback dial as Lugdunum's callback.
enum class RendezvousPath
{
	ServerPunch,
	LugdunumCallback
};

// Covers the 30 s punch window, the 3 s dial back and the stream's handshake.
constexpr std::uint64_t kRendezvousOutcomeTtlMs = 60'000;
constexpr std::size_t kMaxPendingRendezvous = 64;

// Rendezvous we acted on, each waiting for a verified hello from its peer, on either side: the
// target of a punch usually waits for the requester's stream rather than dialling.
class RendezvousOutcomes
{
public:
	// True for a new rendezvous, the attempt to count; a resend or a second notice is the same one.
	bool NoteAttempt(
		RendezvousPath path, std::uint32_t ip, std::optional<Hash> peerHash, std::uint64_t now)
	{
		Evict(now);
		// Peers behind one address are told apart by the hash the rendezvous named.
		const auto same = std::find_if(m_pending.begin(), m_pending.end(), [&](const Entry &entry) {
			return entry.path == path && entry.ip == ip && entry.peerHash == peerHash;
		});
		if (same != m_pending.end()) {
			return false;
		}
		if (m_pending.size() >= kMaxPendingRendezvous) {
			m_pending.erase(m_pending.begin());
		}
		m_pending.push_back({ path, ip, peerHash, now });
		return true;
	}
	// The path of the rendezvous a verified hello completes, taken once. A rendezvous that named the
	// peer's hash matches only that hash; one that did not matches by address.
	std::optional<RendezvousPath> TakeSuccess(std::uint32_t ip, const Hash &peerHash, std::uint64_t now)
	{
		Evict(now);
		auto match =
			std::find_if(m_pending.begin(), m_pending.end(), [ip, &peerHash](const Entry &entry) {
				return entry.ip == ip && entry.peerHash == peerHash;
			});
		if (match == m_pending.end()) {
			match = std::find_if(m_pending.begin(), m_pending.end(), [ip](const Entry &entry) {
				return entry.ip == ip && !entry.peerHash;
			});
		}
		if (match == m_pending.end()) {
			return std::nullopt;
		}
		const RendezvousPath path = match->path;
		m_pending.erase(match);
		return path;
	}
	std::size_t Size() const { return m_pending.size(); }

private:
	struct Entry
	{
		RendezvousPath path;
		std::uint32_t ip;
		std::optional<Hash> peerHash;
		std::uint64_t notedAt;
	};
	void Evict(std::uint64_t now)
	{
		const auto firstLive =
			std::remove_if(m_pending.begin(), m_pending.end(), [now](const Entry &entry) {
				return now >= entry.notedAt && now - entry.notedAt >= kRendezvousOutcomeTtlMs;
			});
		m_pending.erase(firstLive, m_pending.end());
	}
	std::vector<Entry> m_pending;
};
// Targets we sent a Lugdunum callback request for. A callback naming one as its requester is the
// reverse of our own request, whose attempt is already counted.
class RequestedCallbackTargets
{
public:
	void Note(std::uint32_t targetId, std::uint64_t now)
	{
		Evict(now);
		const auto same = Find(targetId);
		if (same != m_entries.end()) {
			same->notedAt = now;
			return;
		}
		if (m_entries.size() >= kMaxPendingRendezvous) {
			m_entries.erase(m_entries.begin());
		}
		m_entries.push_back({ targetId, now });
	}
	bool Take(std::uint32_t targetId, std::uint64_t now)
	{
		Evict(now);
		const auto match = Find(targetId);
		if (match == m_entries.end()) {
			return false;
		}
		m_entries.erase(match);
		return true;
	}
	std::size_t Size() const { return m_entries.size(); }

private:
	struct Entry
	{
		std::uint32_t targetId;
		std::uint64_t notedAt;
	};
	std::vector<Entry>::iterator Find(std::uint32_t targetId)
	{
		return std::find_if(m_entries.begin(), m_entries.end(), [targetId](const Entry &entry) {
			return entry.targetId == targetId;
		});
	}
	void Evict(std::uint64_t now)
	{
		const auto firstLive =
			std::remove_if(m_entries.begin(), m_entries.end(), [now](const Entry &entry) {
				return now >= entry.notedAt && now - entry.notedAt >= kRendezvousOutcomeTtlMs;
			});
		m_entries.erase(firstLive, m_entries.end());
	}
	std::vector<Entry> m_entries;
};
// A Lugdunum callback we requested for one source: the target dials us, so the requester sees the
// success only as that source's hello. Kept on the source, which the hello attaches to.
class PendingCallbackRequest
{
public:
	void NoteSent(std::uint64_t now) { m_sentAt = now; }
	// True once for a hello within the window of the last request.
	bool TakeAnswered(std::uint64_t now)
	{
		const std::optional<std::uint64_t> sentAt = std::exchange(m_sentAt, std::nullopt);
		return sentAt && now >= *sentAt && now - *sentAt < kRendezvousOutcomeTtlMs;
	}

private:
	std::optional<std::uint64_t> m_sentAt;
};

// Cross-server NAT callback (ed2k-server 0.9.86): a LowID asks the server of a LowID source on
// another server to wake that source, which then punches towards us and waits to be dialled.

// OP_NAT_CALLBACKREQUEST to a foreign server: <target_id 4><0 4><crypt 1>. Requester ID 0 marks it
// foreign; crypt is our (CT_SERVER_FLAGS >> 9) & 7.
inline std::array<std::uint8_t, 9> BuildForeignCallbackRequest(std::uint32_t targetId, std::uint8_t crypt)
{
	std::array<std::uint8_t, 9> bytes{};
	Detail::WriteLE(bytes.data(), targetId, 4);
	bytes[8] = crypt;
	return bytes;
}

struct ForeignCallbackAnswer
{
	std::uint32_t targetIp = 0;
	std::uint16_t targetUdpPort = 0;
	std::uint32_t targetId = 0;
	std::uint8_t crypt = 0;
	Hash targetHash{};
};

// The server's 0xA7 answer: <target_ip 4><target_udp_port 2><target_id 4><crypt 1><hash 16>. It
// always carries the hash, so any other length is not an answer to our request.
inline std::optional<ForeignCallbackAnswer> ParseForeignCallbackAnswer(
	const std::uint8_t *data, std::size_t size)
{
	if (data == nullptr || size != 27) {
		return std::nullopt;
	}
	ForeignCallbackAnswer answer;
	answer.targetIp = PeekUInt32(data);
	answer.targetUdpPort = PeekUInt16(data + 4);
	answer.targetId = PeekUInt32(data + 6);
	answer.crypt = data[10];
	std::copy_n(data + 11, answer.targetHash.size(), answer.targetHash.begin());
	return answer;
}

struct ForeignRequestFacts
{
	std::uint32_t ourId = 0;
	std::uint32_t sourceId = 0;
	bool sourceServerIsOurs = false;
	bool serverTakesForeignCallback = false;
	// We announce the hole-punch extension (ShouldAnnounceHolePunchExtension()).
	bool extensionAnnounced = false;
	std::uint16_t ourUdpPort = 0;
	bool weRequireCryptLayer = false;
	bool sourceRequiresCryptLayer = false;
};

// Both LowID on different servers, the source's server takes the request, and the source is odd: on
// both server families only a client that set 0x1000, which a cross-server wake needs, is odd.
inline bool ShouldRequestForeignCallback(const ForeignRequestFacts &facts)
{
	// The server answers nothing when one side requires the crypt layer (ed2k-server#40).
	return facts.extensionAnnounced && !facts.sourceServerIsOurs && facts.serverTakesForeignCallback &&
	       facts.ourUdpPort != 0 && !facts.weRequireCryptLayer && !facts.sourceRequiresCryptLayer &&
	       IsLowID(facts.ourId) && IsLowID(facts.sourceId) && (facts.sourceId & 1) != 0;
}

// Each server allows a burst of 10, then 1/s per source IP, and 60 targets per IP in 10 min; we stay
// below that on each server we ask.
constexpr std::uint32_t kForeignRequestBurst = 5;
constexpr std::uint64_t kForeignRequestRefillMs = 2'000;
constexpr std::size_t kForeignTargetsPerWindow = 30;
constexpr std::uint64_t kForeignTargetWindowMs = 600'000;
// The server answers within 3 s and resends at +1.2 s and +3 s.
constexpr std::uint64_t kForeignRequestTimeoutMs = 30'000;
// An answered request's key outlives the +3 s resend, so the copies decrypt and are dropped here.
constexpr std::uint64_t kForeignAnswerResendMs = 5'000;
// A target that never answered (Lugdunum-only, gone) is not asked again for a while, so source
// re-asks do not use up the server's per-IP target budget.
constexpr std::uint64_t kForeignTargetCooldownMs = 600'000;
constexpr std::size_t kMaxForeignPending = 32;
constexpr std::size_t kMaxForeignServers = 64;
constexpr std::size_t kMaxForeignCooldowns = 256;

// Cross-server requests in flight, their per-server limits, and the cooldown of unanswered targets.
class ForeignCallbackRequests
{
public:
	struct Pending
	{
		std::uint16_t sourceTcpPort = 0;
	};

	// True when the request may go out now; it is then pending for its answer.
	bool TryStart(std::uint32_t serverIp,
		std::uint32_t serverKeyUdp,
		std::uint32_t targetId,
		std::uint16_t sourceTcpPort,
		std::optional<Hash> knownHash,
		std::uint64_t now)
	{
		Expire(now);
		if (FindPending(serverIp, targetId) != m_pending.end() ||
			IsCoolingDown(serverIp, targetId, now) || m_pending.size() >= kMaxForeignPending) {
			return false;
		}
		ServerBudget &budget = BudgetFor(serverIp, now);
		Refill(budget, now);
		PruneTargets(budget, now);
		const bool knownTarget = std::any_of(budget.targets.begin(),
			budget.targets.end(),
			[targetId](const TargetSeen &seen) { return seen.targetId == targetId; });
		if (budget.tokens == 0 ||
			(!knownTarget && budget.targets.size() >= kForeignTargetsPerWindow)) {
			return false;
		}
		--budget.tokens;
		if (!knownTarget) {
			budget.targets.push_back({ targetId, now });
		}
		m_pending.push_back({ serverIp, serverKeyUdp, targetId, sourceTcpPort, knownHash, now });
		return true;
	}

	// The pending request an answer completes, taken once: a resent copy finds nothing. An answer
	// whose hash differs from the source's known one is dropped and the request stays pending. The
	// target then cools down, so a dial that fails is not asked again on the next retry.
	std::optional<Pending> TakeAnswer(
		std::uint32_t serverIp, std::uint32_t targetId, const Hash &targetHash, std::uint64_t now)
	{
		Expire(now);
		const auto match = FindPending(serverIp, targetId);
		if (match == m_pending.end() || (match->knownHash && *match->knownHash != targetHash)) {
			return std::nullopt;
		}
		const Pending taken{ match->sourceTcpPort };
		AddCooldown(serverIp, targetId, now + kForeignTargetCooldownMs);
		if (m_answeredKeys.size() >= kMaxForeignPending) {
			m_answeredKeys.pop_front();
		}
		m_answeredKeys.push_back({ serverIp, match->serverKeyUdp, now + kForeignAnswerResendMs });
		m_pending.erase(match);
		return taken;
	}

	// Whether TakeAnswer() would take this answer, without taking it.
	bool AnswersPending(
		std::uint32_t serverIp, std::uint32_t targetId, const Hash &targetHash, std::uint64_t now)
	{
		Expire(now);
		const auto match = FindPending(serverIp, targetId);
		return match != m_pending.end() && (!match->knownHash || *match->knownHash == targetHash);
	}

	// The UDP keys our pending or just-answered requests to that server IP were sent with, each
	// once. Two listed servers can share an IP with different ports, and so different keys.
	std::vector<std::uint32_t> PendingKeysFrom(std::uint32_t serverIp, std::uint64_t now)
	{
		Expire(now);
		std::vector<std::uint32_t> keys;
		const auto add = [&keys, serverIp](std::uint32_t ip, std::uint32_t key) {
			if (ip == serverIp && std::find(keys.begin(), keys.end(), key) == keys.end()) {
				keys.push_back(key);
			}
		};
		for (const PendingEntry &entry : m_pending) {
			add(entry.serverIp, entry.serverKeyUdp);
		}
		for (const AnsweredKey &answered : m_answeredKeys) {
			add(answered.serverIp, answered.serverKeyUdp);
		}
		return keys;
	}

private:
	struct PendingEntry
	{
		std::uint32_t serverIp;
		std::uint32_t serverKeyUdp;
		std::uint32_t targetId;
		std::uint16_t sourceTcpPort;
		std::optional<Hash> knownHash;
		std::uint64_t sentAt;
	};
	struct TargetSeen
	{
		std::uint32_t targetId;
		std::uint64_t at;
	};
	struct ServerBudget
	{
		std::uint32_t serverIp;
		std::uint32_t tokens;
		std::uint64_t refilledAt;
		std::deque<TargetSeen> targets;
	};
	struct Cooldown
	{
		std::uint32_t serverIp;
		std::uint32_t targetId;
		std::uint64_t until;
	};
	struct AnsweredKey
	{
		std::uint32_t serverIp;
		std::uint32_t serverKeyUdp;
		std::uint64_t until;
	};

	std::vector<PendingEntry>::iterator FindPending(std::uint32_t serverIp, std::uint32_t targetId)
	{
		return std::find_if(
			m_pending.begin(), m_pending.end(), [serverIp, targetId](const PendingEntry &entry) {
				return entry.serverIp == serverIp && entry.targetId == targetId;
			});
	}

	bool IsCoolingDown(std::uint32_t serverIp, std::uint32_t targetId, std::uint64_t now) const
	{
		return std::any_of(m_cooldowns.begin(), m_cooldowns.end(), [&](const Cooldown &cooldown) {
			return cooldown.serverIp == serverIp && cooldown.targetId == targetId &&
			       now < cooldown.until;
		});
	}

	// Unanswered requests expire into a cooldown for their target.
	void Expire(std::uint64_t now)
	{
		m_cooldowns.erase(std::remove_if(m_cooldowns.begin(),
					  m_cooldowns.end(),
					  [now](const Cooldown &cooldown) { return now >= cooldown.until; }),
			m_cooldowns.end());
		while (!m_answeredKeys.empty() && now >= m_answeredKeys.front().until) {
			m_answeredKeys.pop_front();
		}
		for (auto it = m_pending.begin(); it != m_pending.end();) {
			if (now >= it->sentAt && now - it->sentAt >= kForeignRequestTimeoutMs) {
				AddCooldown(it->serverIp,
					it->targetId,
					it->sentAt + kForeignRequestTimeoutMs + kForeignTargetCooldownMs);
				it = m_pending.erase(it);
			} else {
				++it;
			}
		}
	}

	void AddCooldown(std::uint32_t serverIp, std::uint32_t targetId, std::uint64_t until)
	{
		if (m_cooldowns.size() >= kMaxForeignCooldowns) {
			m_cooldowns.pop_front();
		}
		m_cooldowns.push_back({ serverIp, targetId, until });
	}

	ServerBudget &BudgetFor(std::uint32_t serverIp, std::uint64_t now)
	{
		const auto found = std::find_if(m_budgets.begin(),
			m_budgets.end(),
			[serverIp](const ServerBudget &budget) { return budget.serverIp == serverIp; });
		if (found != m_budgets.end()) {
			return *found;
		}
		if (m_budgets.size() >= kMaxForeignServers) {
			m_budgets.pop_front();
		}
		m_budgets.push_back({ serverIp, kForeignRequestBurst, now, {} });
		return m_budgets.back();
	}

	static void Refill(ServerBudget &budget, std::uint64_t now)
	{
		if (now < budget.refilledAt) {
			return;
		}
		const std::uint64_t earned = (now - budget.refilledAt) / kForeignRequestRefillMs;
		if (earned == 0) {
			return;
		}
		budget.tokens = static_cast<std::uint32_t>(
			std::min<std::uint64_t>(kForeignRequestBurst, budget.tokens + earned));
		budget.refilledAt += earned * kForeignRequestRefillMs;
	}

	static void PruneTargets(ServerBudget &budget, std::uint64_t now)
	{
		while (!budget.targets.empty() && now >= budget.targets.front().at &&
			now - budget.targets.front().at >= kForeignTargetWindowMs) {
			budget.targets.pop_front();
		}
	}

	std::vector<PendingEntry> m_pending;
	std::deque<ServerBudget> m_budgets;
	std::deque<Cooldown> m_cooldowns;
	std::deque<AnsweredKey> m_answeredKeys;
};
} // namespace NatServerHolePunch
#endif // ENABLE_NATT_SERVER_COORDINATION
#endif // AMULE_NAT_SERVER_HOLE_PUNCH_H
