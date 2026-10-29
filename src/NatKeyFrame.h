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

#ifndef AMULE_NAT_KEY_FRAME_H
#define AMULE_NAT_KEY_FRAME_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <utility>

/**
 * The NAT-T key frame: a NAT-T frame (type 0xFF) carrying the sender's 16-byte user hash, which
 * tells the receiver what to obfuscate its datagrams to that endpoint with.
 *
 * Every container is bounded and takes the clock as an argument, so a flood of endpoints can neither
 * grow them nor depend on wall time. Neither persists anything: a hash held here selects an
 * obfuscation key for one live exchange and is not an identity (the hello stays authoritative).
 */
namespace NatKeyFrame
{
constexpr std::size_t kHashBytes = 16;
//! A learned peer key (including a server-vouched one) lives until 120 s without outbound use.
constexpr std::uint64_t kPeerKeyIdleMs = 120000;
//! Expected endpoints authorize unauthenticated key-frame announcements for 30 s.
constexpr std::uint64_t kExpectedPeerTtlMs = 30000;
using Hash = std::array<std::uint8_t, kHashBytes>;

inline Hash BuildPayload(const Hash &userHash)
{
	return userHash;
}

//! At least a hash's worth of bytes; surplus is ignored, as eMuleAI does. An all-zero hash is its
//! marker for "no key", so it is not accepted as one.
inline std::optional<Hash> ParsePayload(const std::uint8_t *payload, std::size_t length)
{
	if (payload == nullptr || length < kHashBytes) {
		return std::nullopt;
	}
	Hash hash;
	std::copy_n(payload, kHashBytes, hash.begin());
	if (std::all_of(hash.begin(), hash.end(), [](std::uint8_t byte) { return byte == 0; })) {
		return std::nullopt;
	}
	return hash;
}

namespace Detail
{
inline std::uint64_t EndpointKey(std::uint32_t ip, std::uint16_t port)
{
	return (static_cast<std::uint64_t>(ip) << 16) | port;
}

//! Tolerates a clock that moves backwards: an entry is then not yet expired.
inline bool Elapsed(std::uint64_t now, std::uint64_t since, std::uint64_t window)
{
	return now >= since && now - since >= window;
}

template <typename Map> void EvictOldest(Map &entries)
{
	auto oldest = entries.begin();
	for (auto it = entries.begin(); it != entries.end(); ++it) {
		if (it->second.stamp < oldest->second.stamp) {
			oldest = it;
		}
	}
	entries.erase(oldest);
}

template <typename Map> void PurgeElapsed(Map &entries, std::uint64_t now, std::uint64_t window)
{
	for (auto it = entries.begin(); it != entries.end();) {
		it = Elapsed(now, it->second.stamp, window) ? entries.erase(it) : std::next(it);
	}
}

//! Makes room for one more entry: expired ones first, then the oldest.
template <typename Map>
void MakeRoom(Map &entries, std::size_t capacity, std::uint64_t now, std::uint64_t window)
{
	PurgeElapsed(entries, now, window);
	while (entries.size() >= capacity) {
		EvictOldest(entries);
	}
}
} // namespace Detail

/**
 * Hash a peer is known by for its endpoint, kept only while that endpoint stays in use.
 *
 * Server-vouched identity outranks the identity we dialled, which outranks an unauthenticated key-frame
 * announcement. Only announced hashes may replace earlier announced hashes. Lookup activity refreshes
 * the common idle lifetime; neither a conflicting dial nor an untrusted announcement extends a voucher.
 */
class CPeerKeyCache
{
public:
	CPeerKeyCache(std::size_t capacity, std::uint64_t idleMs)
	: m_capacity(capacity)
	, m_idleMs(idleMs)
	{
	}

	//! The identity we dialled, unless a live server-vouched hash is more authoritative.
	void Learn(std::uint32_t ip, std::uint16_t port, const Hash &hash, std::uint64_t nowMs)
	{
		const auto existing = m_entries.find(Detail::EndpointKey(ip, port));
		if (existing != m_entries.end() && existing->second.source == Source::Vouched &&
			!Detail::Elapsed(nowMs, existing->second.stamp, m_idleMs)) {
			return;
		}
		Store(ip, port, hash, nowMs, Source::Dialled);
	}

	//! A current server rendezvous is authoritative over dialled and announced hashes.
	void LearnVouched(std::uint32_t ip, std::uint16_t port, const Hash &hash, std::uint64_t nowMs)
	{
		Store(ip, port, hash, nowMs, Source::Vouched);
	}

	//! False when a live dialled or server-vouched hash is already known for this endpoint.
	bool LearnAnnounced(std::uint32_t ip, std::uint16_t port, const Hash &hash, std::uint64_t nowMs)
	{
		const auto existing = m_entries.find(Detail::EndpointKey(ip, port));
		if (existing != m_entries.end() && existing->second.source != Source::Announced &&
			!Detail::Elapsed(nowMs, existing->second.stamp, m_idleMs)) {
			return false;
		}
		return Store(ip, port, hash, nowMs, Source::Announced);
	}

	//! A hit counts as use and extends the entry's life.
	std::optional<Hash> Lookup(std::uint32_t ip, std::uint16_t port, std::uint64_t nowMs)
	{
		auto found = m_entries.find(Detail::EndpointKey(ip, port));
		if (found == m_entries.end()) {
			return std::nullopt;
		}
		if (Detail::Elapsed(nowMs, found->second.stamp, m_idleMs)) {
			m_entries.erase(found);
			return std::nullopt;
		}
		found->second.stamp = nowMs;
		return found->second.hash;
	}

	void Forget(std::uint32_t ip, std::uint16_t port) { m_entries.erase(Detail::EndpointKey(ip, port)); }

	std::size_t Size() const { return m_entries.size(); }

private:
	enum class Source
	{
		Announced,
		Dialled,
		Vouched
	};

	struct Entry
	{
		Hash hash;
		std::uint64_t stamp;
		Source source;
	};

	bool Store(std::uint32_t ip, std::uint16_t port, const Hash &hash, std::uint64_t nowMs, Source source)
	{
		if (m_capacity == 0) {
			return false;
		}
		const std::uint64_t key = Detail::EndpointKey(ip, port);
		if (m_entries.find(key) == m_entries.end()) {
			Detail::MakeRoom(m_entries, m_capacity, nowMs, m_idleMs);
		}
		m_entries[key] = Entry{ hash, nowMs, source };
		return true;
	}

	std::size_t m_capacity;
	std::uint64_t m_idleMs;
	std::map<std::uint64_t, Entry> m_entries;
};

//! At most one key frame per endpoint per window, so a peer cannot make us send them in volume.
class CSendGate
{
public:
	CSendGate(std::size_t capacity, std::uint64_t windowMs)
	: m_capacity(capacity)
	, m_windowMs(windowMs)
	{
	}

	bool TryAcquire(std::uint32_t ip, std::uint16_t port, std::uint64_t nowMs)
	{
		if (m_capacity == 0) {
			return false;
		}
		const std::uint64_t key = Detail::EndpointKey(ip, port);
		auto existing = m_entries.find(key);
		if (existing != m_entries.end()) {
			if (!Detail::Elapsed(nowMs, existing->second.stamp, m_windowMs)) {
				return false;
			}
			existing->second.stamp = nowMs;
			return true;
		}
		Detail::MakeRoom(m_entries, m_capacity, nowMs, m_windowMs);
		m_entries[key] = Entry{ nowMs };
		return true;
	}

	void Forget(std::uint32_t ip, std::uint16_t port) { m_entries.erase(Detail::EndpointKey(ip, port)); }

	std::size_t Size() const { return m_entries.size(); }

private:
	struct Entry
	{
		std::uint64_t stamp;
	};

	std::size_t m_capacity;
	std::uint64_t m_windowMs;
	std::map<std::uint64_t, Entry> m_entries;
};

//! Endpoints a rendezvous told us a peer will contact us from. Only these may announce a key.
class CExpectedPeers
{
public:
	enum class Result
	{
		Accepted,
		ConflictingVoucher
	};
	CExpectedPeers(std::size_t capacity, std::uint64_t ttlMs)
	: m_capacity(capacity)
	, m_ttlMs(ttlMs)
	{
	}

	Result Expect(std::uint32_t ip,
		std::uint16_t port,
		std::uint64_t nowMs,
		std::optional<Hash> vouchedHash = std::nullopt)
	{
		if (m_capacity == 0) {
			return Result::ConflictingVoucher;
		}
		const std::uint64_t key = Detail::EndpointKey(ip, port);
		auto existing = m_entries.find(key);
		if (existing != m_entries.end() && !Detail::Elapsed(nowMs, existing->second.stamp, m_ttlMs)) {
			if (existing->second.conflicted) {
				existing->second.stamp = nowMs;
				return Result::ConflictingVoucher;
			}
			if (existing->second.vouchedHash && vouchedHash &&
				*existing->second.vouchedHash != *vouchedHash) {
				existing->second.stamp = nowMs;
				existing->second.vouchedHash.reset();
				existing->second.conflicted = true;
				return Result::ConflictingVoucher;
			}
			if (!vouchedHash) {
				vouchedHash = existing->second.vouchedHash;
			}
		} else if (existing == m_entries.end()) {
			Detail::MakeRoom(m_entries, m_capacity, nowMs, m_ttlMs);
		}
		m_entries[key] = Entry{ nowMs, vouchedHash, false };
		return Result::Accepted;
	}

	bool IsExpected(std::uint32_t ip, std::uint16_t port, std::uint64_t nowMs) const
	{
		const auto found = m_entries.find(Detail::EndpointKey(ip, port));
		return found != m_entries.end() && !Detail::Elapsed(nowMs, found->second.stamp, m_ttlMs);
	}

	bool HasVouchedHash(std::uint32_t ip, std::uint16_t port, std::uint64_t nowMs) const
	{
		const auto found = m_entries.find(Detail::EndpointKey(ip, port));
		return found != m_entries.end() && found->second.vouchedHash.has_value() &&
		       !Detail::Elapsed(nowMs, found->second.stamp, m_ttlMs);
	}

	bool HasConflictingVoucher(std::uint32_t ip, std::uint16_t port, std::uint64_t nowMs) const
	{
		const auto found = m_entries.find(Detail::EndpointKey(ip, port));
		return found != m_entries.end() && found->second.conflicted &&
		       !Detail::Elapsed(nowMs, found->second.stamp, m_ttlMs);
	}

	std::size_t Size() const { return m_entries.size(); }

private:
	struct Entry
	{
		std::uint64_t stamp;
		std::optional<Hash> vouchedHash;
		bool conflicted;
	};

	std::size_t m_capacity;
	std::uint64_t m_ttlMs;
	std::map<std::uint64_t, Entry> m_entries;
};

//! What to do with one outgoing QUIC datagram.
struct SendPlan
{
	//! Hash to obfuscate the datagram with; empty sends it plain.
	std::optional<Hash> key;
	bool sendKeyFrame = false;
};

//! QUIC long headers (Initial, Handshake) have the top bit set; short-header packets do not.
inline bool IsQuicLongHeader(const std::uint8_t *datagram, std::size_t length)
{
	return datagram != nullptr && length >= 1 && (datagram[0] & 0x80) != 0;
}

/**
 * Obfuscate with the peer's hash whenever it is known. Our own key frame precedes a long header, once
 * per window, and only where it can itself be obfuscated: eMuleAI also sends it in the clear when it
 * has no key, which would publish our user hash to anyone on the path.
 */
inline SendPlan PlanDatagram(CPeerKeyCache &keys,
	CSendGate &gate,
	std::uint32_t ip,
	std::uint16_t port,
	bool longHeader,
	std::uint64_t nowMs)
{
	SendPlan plan;
	plan.key = keys.Lookup(ip, port, nowMs);
	plan.sendKeyFrame = longHeader && plan.key.has_value() && gate.TryAcquire(ip, port, nowMs);
	return plan;
}

enum class KeyFrameResult
{
	//! The runtime opt-in is off: a datagram nobody asked for changes nothing.
	Disabled,
	Malformed,
	//! No rendezvous told us this endpoint would contact us.
	Unexpected,
	//! We already hold the identity we dialled for this endpoint.
	Held,
	Learned
};

struct Callbacks
{
	//! The runtime opt-in for QUIC NAT-T.
	std::function<bool()> enabled;
	//! Whether the user allows the UDP obfuscation layer at all.
	std::function<bool()> obfuscationOn;
	//! Sends our key frame, obfuscated with @a key; false means the sink rejected it.
	std::function<bool(std::uint32_t ip, std::uint16_t port, const Hash &key)> sendKeyFrame;
	//! Sends one QUIC datagram, obfuscated with @a key unless it is null.
	//! False means the datagram was not accepted by the sink.
	std::function<bool(const std::uint8_t *payload,
		std::size_t length,
		std::uint32_t ip,
		std::uint16_t port,
		const Hash *key)>
		sendDatagram;
};

/**
 * Sends QUIC datagrams and handles key frames, with everything that touches a socket or a preference
 * behind a callback so it can be driven by fakes.
 */
class CKeyFrameService
{
public:
	explicit CKeyFrameService(Callbacks callbacks)
	: m_callbacks(std::move(callbacks))
	{
	}

	//! A rendezvous says this peer is about to contact us.
	void ExpectPeer(std::uint32_t ip,
		std::uint16_t port,
		std::uint64_t nowMs,
		std::optional<Hash> vouchedHash = std::nullopt)
	{
		const auto result = m_expected.Expect(ip, port, nowMs, vouchedHash);
		if (result == CExpectedPeers::Result::ConflictingVoucher) {
			m_keys.Forget(ip, port);
		} else if (vouchedHash) {
			m_keys.LearnVouched(ip, port, *vouchedHash, nowMs);
		}
	}

	//! We are dialling this identity. It is authoritative unless a live server-vouched key exists.
	void NoteDialled(std::uint32_t ip, std::uint16_t port, const Hash &hash, std::uint64_t nowMs)
	{
		if (!m_expected.HasConflictingVoucher(ip, port, nowMs)) {
			m_keys.Learn(ip, port, hash, nowMs);
		}
	}

	KeyFrameResult OnKeyFrame(const std::uint8_t *payload,
		std::size_t length,
		std::uint32_t ip,
		std::uint16_t port,
		std::uint64_t nowMs)
	{
		if (!m_callbacks.enabled()) {
			return KeyFrameResult::Disabled;
		}
		const std::optional<Hash> hash = ParsePayload(payload, length);
		if (!hash) {
			return KeyFrameResult::Malformed;
		}
		if (!m_expected.IsExpected(ip, port, nowMs)) {
			return KeyFrameResult::Unexpected;
		}
		if (m_expected.HasConflictingVoucher(ip, port, nowMs) ||
			m_expected.HasVouchedHash(ip, port, nowMs)) {
			return KeyFrameResult::Held;
		}
		return m_keys.LearnAnnounced(ip, port, *hash, nowMs) ? KeyFrameResult::Learned
								     : KeyFrameResult::Held;
	}

	bool SendDatagram(const std::uint8_t *payload,
		std::size_t length,
		std::uint32_t ip,
		std::uint16_t port,
		std::uint64_t nowMs)
	{
		// A conflicting server identity makes even plaintext unsafe (it exposes QUIC's ALPN).
		// Fail the sink send rather than silently downgrade or queue any frame.
		if (m_expected.HasConflictingVoucher(ip, port, nowMs)) {
			return false;
		}
		SendPlan plan;
		if (m_callbacks.obfuscationOn()) {
			plan = PlanDatagram(
				m_keys, m_gate, ip, port, IsQuicLongHeader(payload, length), nowMs);
		}
		if (plan.sendKeyFrame && !m_callbacks.sendKeyFrame(ip, port, *plan.key)) {
			m_gate.Forget(ip, port);
			return false;
		}
		const bool accepted =
			m_callbacks.sendDatagram(payload, length, ip, port, plan.key ? &*plan.key : nullptr);
		if (!accepted) {
			if (plan.sendKeyFrame) {
				m_gate.Forget(ip, port);
			}
		}
		return accepted;
	}

private:
	Callbacks m_callbacks;
	CPeerKeyCache m_keys{ 256, kPeerKeyIdleMs };
	CSendGate m_gate{ 256, 10000 };
	CExpectedPeers m_expected{ 256, kExpectedPeerTtlMs };
};
} // namespace NatKeyFrame

#endif // AMULE_NAT_KEY_FRAME_H
