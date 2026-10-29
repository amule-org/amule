//
// Pure bounded ledger for confirmed eMuleAI EAQC capability requests.
//
#ifndef QUIC_CAPS_REQUEST_LEDGER_H
#define QUIC_CAPS_REQUEST_LEDGER_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

struct QuicCapsSendPlan
{
	bool obfuscate = false;
	std::array<uint8_t, 16> targetHash{};
};

//! Plaintext, with no target hash, without a public IP or with obfuscation off.
inline QuicCapsSendPlan MakeQuicCapsSendPlan(
	bool clientCryptLayerSupported, uint32_t publicIP, const std::array<uint8_t, 16> &targetHash) noexcept
{
	QuicCapsSendPlan plan;
	plan.obfuscate =
		clientCryptLayerSupported && publicIP != 0 && targetHash != std::array<uint8_t, 16>{};
	if (plan.obfuscate) {
		plan.targetHash = targetHash;
	}
	return plan;
}

struct QuicCapsRequestRecord
{
	std::array<uint8_t, 16> peer;
	uint32_t ipv4;
	uint16_t udpPort;
	std::array<uint8_t, 16> fileHash;
	uint64_t expiresAt;
};

class CQuicCapsRequestLedger
{
public:
	explicit CQuicCapsRequestLedger(std::size_t maximum)
	: m_maximum(maximum)
	{
	}

	// Records one request. The oldest record is evicted when the bound is full.
	// A request with the same peer and endpoint identity is not recorded twice;
	// the file hash is retained for wire bookkeeping but is informational on ACK.
	bool Add(const std::array<uint8_t, 16> &peer,
		uint32_t ipv4,
		uint16_t udpPort,
		const std::array<uint8_t, 16> &fileHash,
		uint64_t nowMs)
	{
		if (m_maximum == 0 || peer == std::array<uint8_t, 16>{} || ipv4 == 0 || udpPort == 0)
			return false;
		Expire(nowMs);
		if (Find(peer, ipv4, udpPort) != m_records.end())
			return false;
		if (m_records.size() == m_maximum)
			m_records.erase(m_records.begin());
		m_records.push_back({ peer, ipv4, udpPort, fileHash, Expiration(nowMs) });
		return true;
	}

	// Matches and removes exactly one unexpired request.
	bool Consume(const std::array<uint8_t, 16> &peer,
		uint32_t ipv4,
		uint16_t udpPort,
		const std::array<uint8_t, 16> & /*fileHash*/,
		uint64_t nowMs)
	{
		auto found = Find(peer, ipv4, udpPort);
		if (found == m_records.end() || nowMs >= found->expiresAt)
			return false;
		m_records.erase(found);
		return true;
	}

	void Expire(uint64_t nowMs)
	{
		for (auto it = m_records.begin(); it != m_records.end();) {
			if (nowMs >= it->expiresAt)
				it = m_records.erase(it);
			else
				++it;
		}
	}

	std::size_t Size(uint64_t nowMs)
	{
		Expire(nowMs);
		return m_records.size();
	}

private:
	using Iterator = std::vector<QuicCapsRequestRecord>::iterator;

	Iterator Find(const std::array<uint8_t, 16> &peer, uint32_t ipv4, uint16_t udpPort)
	{
		for (auto it = m_records.begin(); it != m_records.end(); ++it)
			if (it->peer == peer && it->ipv4 == ipv4 && it->udpPort == udpPort)
				return it;
		return m_records.end();
	}

	static uint64_t Expiration(uint64_t nowMs)
	{
		return nowMs > UINT64_MAX - 30000 ? UINT64_MAX : nowMs + 30000;
	}

	std::size_t m_maximum;
	std::vector<QuicCapsRequestRecord> m_records;
};

#endif
