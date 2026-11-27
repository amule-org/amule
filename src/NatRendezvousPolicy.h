//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//

#ifndef NATRENDEZVOUSPOLICY_H
#define NATRENDEZVOUSPOLICY_H

#include "PeerAddressing.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

/**
 * Pure admission policy for a future NAT rendezvous control path.
 *
 * This deliberately does not define a wire opcode or forward packets. A relay cannot prove that
 * a candidate peer is reachable, so the first slice only validates observed endpoints and bounds
 * request pressure. Production handlers must use the observed source, never a peer-provided hint.
 */
namespace NatRendezvous
{
constexpr std::uint64_t kRequestThrottleMs = 3 * 1000;
constexpr std::size_t kMaxTrackedRequesters = 256;

inline std::optional<PeerAddressing::UdpEndpoint> AcceptObservedEndpoint(
	const PeerAddressing::UdpEndpoint &claimed, const PeerAddressing::UdpEndpoint &observed) noexcept
{
	if (!PeerAddressing::MatchesUdpSource(claimed, observed)) {
		return std::nullopt;
	}
	return observed;
}

class CRequesterLimiter
{
public:
	CRequesterLimiter() = default;
	CRequesterLimiter(std::uint64_t throttleMs, std::size_t maxTracked) noexcept
	: m_throttleMs(throttleMs)
	, m_maxTracked(maxTracked)
	{
	}

	/** @p now is a monotonic millisecond tick; rollback re-anchors the request window. */
	bool Admit(const CNetworkAddress &requester, std::uint64_t now) noexcept
	{
		if (!PeerAddressing::IsSecurityKey(requester)) {
			return false;
		}
		const CNetworkAddress key = PeerAddressing::RateLimitScope(requester);

		Evict(now);
		for (auto &entry : m_entries) {
			if (entry.address == key) {
				if (now >= entry.lastRequest && now - entry.lastRequest < m_throttleMs) {
					return false;
				}
				entry.lastRequest = now;
				return true;
			}
		}

		if (m_entries.size() >= m_maxTracked) {
			const auto oldest = std::min_element(m_entries.begin(),
				m_entries.end(),
				[](const SEntry &left, const SEntry &right) {
					return left.lastRequest < right.lastRequest;
				});
			m_entries.erase(oldest);
		}
		m_entries.push_back({ key, now });
		return true;
	}

private:
	struct SEntry
	{
		CNetworkAddress address;
		std::uint64_t lastRequest;
	};

	void Evict(std::uint64_t now) noexcept
	{
		const auto firstLive =
			std::remove_if(m_entries.begin(), m_entries.end(), [this, now](const SEntry &entry) {
				return now >= entry.lastRequest && now - entry.lastRequest >= m_throttleMs;
			});
		m_entries.erase(firstLive, m_entries.end());
	}

	std::uint64_t m_throttleMs = kRequestThrottleMs;
	std::size_t m_maxTracked = kMaxTrackedRequesters;
	std::vector<SEntry> m_entries;
};

// A rendezvous our buddy forwards stays in flight this long, so eMuleQt's resends (up to three, 5 s
// apart) join it rather than start another burst and sweep.
constexpr std::uint64_t kServedRendezvousInFlightMs = 20 * 1000;
// One new attempt per requester address every 5 s, as eMuleQt allows.
constexpr std::uint64_t kServedRendezvousPerRequesterMs = 5 * 1000;
// The server paths allow 20 actions a minute; this budget is the Kad buddy's own.
constexpr std::uint64_t kServedRendezvousWindowMs = 60 * 1000;
constexpr std::size_t kMaxServedRendezvousPerWindow = 20;

enum class EServedAdmission
{
	Start,
	Join,
	RequesterLimited,
	OverGlobalLimit
};

// Admission for the rendezvous our buddy forwards to us. Each one makes us punch and sweep an
// endpoint the buddy saw, and that source can be forged: the per-requester limit stops repeats to
// one address, the global one a sender that forges a new source each time.
class CServedRendezvousGate
{
public:
	EServedAdmission Admit(const PeerAddressing::UdpEndpoint &requester, std::uint64_t now)
	{
		Evict(now);
		const auto inFlight = std::find_if(
			m_inFlight.begin(), m_inFlight.end(), [&requester](const SAttempt &attempt) {
				return attempt.requester.address == requester.address &&
				       attempt.requester.port == requester.port;
			});
		if (inFlight != m_inFlight.end()) {
			return EServedAdmission::Join;
		}
		if (m_started.size() >= kMaxServedRendezvousPerWindow) {
			return EServedAdmission::OverGlobalLimit;
		}
		if (!m_perRequester.Admit(requester.address, now)) {
			return EServedAdmission::RequesterLimited;
		}
		m_started.push_back(now);
		m_inFlight.push_back({ requester, now });
		return EServedAdmission::Start;
	}

private:
	struct SAttempt
	{
		PeerAddressing::UdpEndpoint requester;
		std::uint64_t startedAt;
	};

	void Evict(std::uint64_t now)
	{
		const auto expired = [now](std::uint64_t at, std::uint64_t window) {
			return now >= at && now - at >= window;
		};
		m_inFlight.erase(std::remove_if(m_inFlight.begin(),
					 m_inFlight.end(),
					 [&expired](const SAttempt &attempt) {
						 return expired(
							 attempt.startedAt, kServedRendezvousInFlightMs);
					 }),
			m_inFlight.end());
		m_started.erase(std::remove_if(m_started.begin(),
					m_started.end(),
					[&expired](std::uint64_t at) {
						return expired(at, kServedRendezvousWindowMs);
					}),
			m_started.end());
	}

	CRequesterLimiter m_perRequester{ kServedRendezvousPerRequesterMs, kMaxTrackedRequesters };
	// Both stay bounded by the global limit.
	std::vector<SAttempt> m_inFlight;
	std::vector<std::uint64_t> m_started;
};

// The requester side follows eMuleQt: up to three sends 5 s apart, an attempt over after 45 s.
constexpr std::size_t kRequesterSends = 3;
constexpr std::uint64_t kRequesterSendIntervalMs = 5 * 1000;
// eMuleAI dials the source's Kad endpoint at once; we give the target's punch this long first.
constexpr std::uint64_t kRequesterFallbackDialMs = 3 * 1000;
constexpr std::uint64_t kRequesterAttemptMs = 45 * 1000;
constexpr std::uint64_t kRequesterCooldownMs = 10 * 60 * 1000;
// A full set refuses a new attempt rather than drop one whose expiry would start its cooldown.
constexpr std::size_t kMaxRequesterAttempts = 32;
constexpr std::size_t kMaxRequesterMarks = 256;

using KadRendezvousHash = std::array<std::uint8_t, 16>;

// A firewalled Kad source we ask for a rendezvous through its buddy. Addresses are IPv4 in aMule's
// byte order.
// A firewalled Kad source's endpoint as a record names it: the IP its storer saw and its Kad port.
struct SKadEndpoint
{
	std::uint32_t ip = 0;
	std::uint16_t port = 0;
	bool operator==(const SKadEndpoint &other) const { return ip == other.ip && port == other.port; }
};

// A source that restarted publishes a new port while nodes still hand out its old records, in any
// order, so the endpoint we keep can be a stale one. Up to this many earlier ones are kept too.
constexpr std::size_t kMaxFormerKadEndpoints = 3;

// @a former once the source's endpoint moves from @a current to @a incoming: newest first.
inline void NoteKadEndpointChange(
	std::vector<SKadEndpoint> &former, const SKadEndpoint &current, const SKadEndpoint &incoming)
{
	if (current == incoming) {
		return;
	}
	former.erase(std::remove(former.begin(), former.end(), incoming), former.end());
	if (current.ip != 0 && current.port != 0) {
		former.erase(std::remove(former.begin(), former.end(), current), former.end());
		former.insert(former.begin(), current);
	}
	if (former.size() > kMaxFormerKadEndpoints) {
		former.resize(kMaxFormerKadEndpoints);
	}
}

struct SKadRendezvousTarget
{
	std::uint32_t ip;
	std::uint16_t udpPort;
	KadRendezvousHash hash;
	std::uint32_t buddyIP;
	std::uint16_t buddyPort;
	// What the send and the dial need; the tracker itself matches on ip, port and hash only.
	std::uint16_t tcpPort = 0;
	KadRendezvousHash buddyID{};
	KadRendezvousHash fileHash{};
	// Endpoints earlier records named: punched too, and a punch from one is the target's.
	std::vector<SKadEndpoint> former{};

	bool Names(std::uint32_t atIP, std::uint16_t atPort) const
	{
		return (ip == atIP && udpPort == atPort) ||
		       std::find(former.begin(), former.end(), SKadEndpoint{ atIP, atPort }) != former.end();
	}
	bool AtAddress(std::uint32_t atIP) const
	{
		return ip == atIP ||
		       std::any_of(former.begin(), former.end(), [atIP](const SKadEndpoint &endpoint) {
			       return endpoint.ip == atIP;
		       });
	}
};

struct SKadRendezvousAction
{
	enum EKind
	{
		Send,
		FallbackDial,
		Expired
	};
	EKind kind;
	SKadRendezvousTarget target;
};

// Our rendezvous requests in flight. The target's first punch is what we dial; a verified hello
// ends the attempt, and one that times out keeps that source from this path for a while. A target
// that answers OP_FILENOTFOUND does not speak the rendezvous and is never asked again.
class CKadRendezvousRequests
{
public:
	bool Start(const SKadRendezvousTarget &target, std::uint64_t now)
	{
		EvictMarks(now);
		if (m_attempts.size() >= kMaxRequesterAttempts || IsMarked(target.hash) ||
			std::any_of(m_attempts.begin(), m_attempts.end(), [&target](const SAttempt &attempt) {
				return attempt.target.hash == target.hash ||
				       (attempt.target.ip == target.ip &&
					       attempt.target.udpPort == target.udpPort);
			})) {
			return false;
		}
		m_attempts.push_back({ target, now, 0, false, false, false });
		return true;
	}

	// Everything due now, each returned once. A send whose moment a slow tick missed is skipped.
	std::vector<SKadRendezvousAction> TakeDue(std::uint64_t now)
	{
		std::vector<SKadRendezvousAction> due;
		for (auto it = m_attempts.begin(); it != m_attempts.end();) {
			const std::uint64_t elapsed = now >= it->startedAt ? now - it->startedAt : 0;
			if (elapsed >= kRequesterAttemptMs) {
				due.push_back({ SKadRendezvousAction::Expired, it->target });
				Mark(it->target.hash, now + kRequesterCooldownMs);
				it = m_attempts.erase(it);
				continue;
			}
			if (!it->punched) {
				const std::size_t sendsDue = std::min<std::size_t>(
					kRequesterSends, elapsed / kRequesterSendIntervalMs + 1);
				if (sendsDue > it->sends) {
					it->sends = sendsDue;
					due.push_back({ SKadRendezvousAction::Send, it->target });
				}
				if (!it->fallbackDialled && elapsed >= kRequesterFallbackDialMs) {
					it->fallbackDialled = true;
					due.push_back({ SKadRendezvousAction::FallbackDial, it->target });
				}
			}
			++it;
		}
		return due;
	}

	// The target to dial for a punch from @a ip, once per attempt. A punch from its own port is
	// the target's; from another it may be a neighbour behind the same address, so only a lone
	// attempt there takes it. Any punch ends the sends and the fallback dial, but the endpoint the
	// fallback already dialled is not dialled again: only a punch from another port is.
	std::optional<SKadRendezvousTarget> TakePunch(std::uint32_t ip, std::uint16_t port, std::uint64_t now)
	{
		const auto match = Find(ip, port);
		if (match == m_attempts.end() || match->punchDialled ||
			now - match->startedAt >= kRequesterAttemptMs) {
			return std::nullopt;
		}
		match->punched = true;
		if (match->fallbackDialled && ip == match->target.ip && port == match->target.udpPort) {
			return std::nullopt;
		}
		match->punchDialled = true;
		return match->target;
	}

	bool Complete(std::uint32_t ip, const KadRendezvousHash &hash)
	{
		const auto match =
			std::find_if(m_attempts.begin(), m_attempts.end(), [&](const SAttempt &attempt) {
				return attempt.target.AtAddress(ip) && attempt.target.hash == hash;
			});
		if (match == m_attempts.end()) {
			return false;
		}
		m_attempts.erase(match);
		return true;
	}

	// An OP_FILENOTFOUND from an attempt's target: it read our 0xA0 as a reask. Nullopt when no
	// attempt is in flight there, so the caller handles the answer as before. @a knownSender is
	// the hash of a known source at exactly that endpoint: only the attempt's own target, which
	// may have connected to us before, ends the attempt; another source answers its own reask.
	std::optional<SKadRendezvousTarget> NoRendezvous(std::uint32_t ip,
		std::uint16_t port,
		const std::optional<KadRendezvousHash> &knownSender = std::nullopt)
	{
		const auto match = Find(ip, port);
		if (match == m_attempts.end() || (knownSender && *knownSender != match->target.hash)) {
			return std::nullopt;
		}
		const SKadRendezvousTarget target = match->target;
		m_attempts.erase(match);
		Mark(target.hash, kForever);
		return target;
	}

	std::size_t Size() const { return m_attempts.size(); }

private:
	static constexpr std::uint64_t kForever = ~std::uint64_t(0);

	struct SAttempt
	{
		SKadRendezvousTarget target;
		std::uint64_t startedAt;
		std::size_t sends;
		bool fallbackDialled;
		bool punched;
		bool punchDialled;
	};
	struct SMark
	{
		KadRendezvousHash hash;
		std::uint64_t until;
	};

	std::vector<SAttempt>::iterator Find(std::uint32_t ip, std::uint16_t port)
	{
		const auto exact = std::find_if(m_attempts.begin(),
			m_attempts.end(),
			[&](const SAttempt &attempt) { return attempt.target.Names(ip, port); });
		if (exact != m_attempts.end()) {
			return exact;
		}
		const auto atAddress = [ip](const SAttempt &attempt) { return attempt.target.AtAddress(ip); };
		if (std::count_if(m_attempts.begin(), m_attempts.end(), atAddress) != 1) {
			return m_attempts.end();
		}
		return std::find_if(m_attempts.begin(), m_attempts.end(), atAddress);
	}
	bool IsMarked(const KadRendezvousHash &hash) const
	{
		return std::any_of(m_marks.begin(), m_marks.end(), [&hash](const SMark &mark) {
			return mark.hash == hash;
		});
	}
	void Mark(const KadRendezvousHash &hash, std::uint64_t until)
	{
		m_marks.erase(std::remove_if(m_marks.begin(),
				      m_marks.end(),
				      [&hash](const SMark &mark) { return mark.hash == hash; }),
			m_marks.end());
		if (m_marks.size() >= kMaxRequesterMarks) {
			// The oldest cooldown goes first: a "no rendezvous" mark lasts the session.
			const auto cooldown = std::find_if(m_marks.begin(),
				m_marks.end(),
				[](const SMark &mark) { return mark.until != kForever; });
			m_marks.erase(cooldown != m_marks.end() ? cooldown : m_marks.begin());
		}
		m_marks.push_back({ hash, until });
	}
	void EvictMarks(std::uint64_t now)
	{
		m_marks.erase(std::remove_if(m_marks.begin(),
				      m_marks.end(),
				      [now](const SMark &mark) { return now >= mark.until; }),
			m_marks.end());
	}

	std::vector<SAttempt> m_attempts;
	std::vector<SMark> m_marks;
};
} // namespace NatRendezvous

#endif // NATRENDEZVOUSPOLICY_H
