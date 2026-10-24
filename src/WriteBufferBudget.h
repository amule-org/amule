// Copyright (c) 2026 aMule Team
// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef WRITEBUFFERBUDGET_H
#define WRITEBUFFERBUDGET_H

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>

// Payload credits cover both requested network data and dirty buffers. A request
// and all its received fragments share a lease: cancellation cannot uncharge data
// still owned by the writer, and a failed write retains its credit for retry.
class WriteBufferBudget
{
	struct Account
	{
		std::string volume;
		uint64_t bytes = 0;
	};
	struct State
	{
		std::mutex mutex;
		std::map<std::string, Account> paths;
		uint64_t used = 0;
	};

public:
	static constexpr uint64_t kGlobalLimit = 64 * 1024 * 1024;
	static constexpr uint64_t kVolumeLimit = 16 * 1024 * 1024;
	class Lease
	{
	public:
		~Lease()
		{
			std::lock_guard<std::mutex> lock(m_state->mutex);
			m_state->used -= m_bytes;
			m_state->paths.at(m_path).bytes -= m_bytes;
		}
		Lease(const Lease &) = delete;
		Lease &operator=(const Lease &) = delete;

		Lease(std::shared_ptr<State> state, std::string path, uint64_t bytes)
		: m_state(std::move(state))
		, m_path(std::move(path))
		, m_bytes(bytes)
		{
		}

	private:
		std::shared_ptr<State> m_state;
		std::string m_path;
		uint64_t m_bytes;
	};
	using Credit = std::shared_ptr<Lease>;

	explicit WriteBufferBudget(uint64_t global = kGlobalLimit, uint64_t volume = kVolumeLimit)
	: m_global(global)
	, m_volume(volume)
	{
	}
	static WriteBufferBudget &Get()
	{
		static WriteBufferBudget budget;
		return budget;
	}

	Credit Reserve(const std::string &path, uint64_t bytes)
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		if (bytes == 0 || bytes > m_global || m_state->used > m_global - bytes) {
			return nullptr;
		}
		const auto found = m_state->paths.find(path);
		const std::string volume = found == m_state->paths.end() ? "" : found->second.volume;
		std::map<std::string, uint64_t> volumes;
		for (const auto &entry : m_state->paths) {
			volumes[entry.second.volume] += entry.second.bytes;
		}
		const uint64_t unknown = volumes[""];
		uint64_t known = 0;
		if (volume.empty()) {
			for (const auto &entry : volumes) {
				if (!entry.first.empty()) {
					known = std::max(known, entry.second);
				}
			}
		} else {
			known = volumes[volume];
		}
		// Unknown paths might alias ANY known volume. Include that debt in every
		// volume's allowance, so learning an identity cannot exceed its limit.
		if (bytes > m_volume || unknown + known > m_volume - bytes) {
			return nullptr;
		}
		auto &account = m_state->paths[path];
		auto lease = std::make_shared<Lease>(m_state, path, bytes);
		account.bytes += bytes;
		m_state->used += bytes;
		return lease;
	}

	// Worker-side discovery only. Never migrate live credits between two known
	// volumes (e.g. a remount); wait until the previous ownership drains.
	bool Identify(const std::string &path, const std::string &volume)
	{
		if (volume.empty()) {
			return false;
		}
		std::lock_guard<std::mutex> lock(m_state->mutex);
		auto &account = m_state->paths[path];
		if (account.bytes != 0 && !account.volume.empty() && account.volume != volume) {
			return false;
		}
		account.volume = volume;
		return true;
	}
	uint64_t Used() const
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		return m_state->used;
	}

private:
	uint64_t m_global;
	uint64_t m_volume;
	std::shared_ptr<State> m_state = std::make_shared<State>();
};
#endif
