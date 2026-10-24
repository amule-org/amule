// Copyright (c) 2026 aMule Team
// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef UPLOADBUFFERING_H
#define UPLOADBUFFERING_H

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

// Read-ahead covers two seconds at the observed slot rate. Keep a block in
// reserve even before the first rate sample, and cap the target at four MiB.
inline uint32_t UploadBufferTarget(uint32_t bytesPerSecond, uint32_t blockSize)
{
	return static_cast<uint32_t>(std::clamp<uint64_t>(
		uint64_t{ bytesPerSecond } * 2 + 1, uint64_t{ blockSize } + 1, 4 * 1024 * 1024));
}

// Conservative application-buffer accounting, not process RSS or kernel socket
// memory. Four payload copies plus 4 KiB per request cover read/copy/compression
// buffers and packet overhead; tiny requests cannot evade the memory budget.
// A reservation follows a block from admission through its last queued packet.
class UploadBufferBudget
{
public:
	static constexpr uint64_t kClientLimit = 16 * 1024 * 1024;
	static constexpr uint64_t kGlobalLimit = 128 * 1024 * 1024;
	struct Account
	{
		std::atomic<uint64_t> used{ 0 };
	};
	class Reservation
	{
	public:
		Reservation(std::shared_ptr<Account> global, std::shared_ptr<Account> client, uint64_t charge)
		: m_global(std::move(global))
		, m_client(std::move(client))
		, m_charge(charge)
		{
		}
		~Reservation()
		{
			m_client->used.fetch_sub(m_charge);
			m_global->used.fetch_sub(m_charge);
		}
		Reservation(const Reservation &) = delete;
		Reservation &operator=(const Reservation &) = delete;

	private:
		std::shared_ptr<Account> m_global;
		std::shared_ptr<Account> m_client;
		uint64_t m_charge;
	};
	using Lease = std::shared_ptr<Reservation>;

	Lease Reserve(const std::shared_ptr<Account> &client, uint32_t payload)
	{
		const uint64_t charge = uint64_t{ payload } * 4 + 4096;
		if (payload == 0 || !Add(*client, charge, kClientLimit)) {
			return {};
		}
		if (!Add(*m_global, charge, kGlobalLimit)) {
			client->used.fetch_sub(charge);
			return {};
		}
		try {
			return std::make_shared<Reservation>(m_global, client, charge);
		} catch (...) {
			client->used.fetch_sub(charge);
			m_global->used.fetch_sub(charge);
			throw;
		}
	}
	uint64_t Used() const { return m_global->used.load(); }

private:
	static bool Add(Account &account, uint64_t charge, uint64_t limit)
	{
		uint64_t used = account.used.load();
		while (charge <= limit && used <= limit - charge) {
			if (account.used.compare_exchange_weak(used, used + charge)) {
				return true;
			}
		}
		return false;
	}
	std::shared_ptr<Account> m_global = std::make_shared<Account>();
};
#endif
