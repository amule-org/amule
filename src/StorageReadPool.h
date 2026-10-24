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
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA
//

#ifndef STORAGEREADPOOL_H
#define STORAGEREADPOOL_H

#include <array>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// Four bounded ownership slots, not an unbounded work queue. A storage group keeps
// its slot through completion delivery, so a stalled drive cannot occupy the pool
// with either running reads or undelivered results. No filesystem calls occur here.
template <typename Job> class StorageReadPool
{
public:
	static constexpr size_t kWorkers = 4;
	struct Completion
	{
		std::unique_ptr<Job> job;
		std::exception_ptr error;
	};

	StorageReadPool(std::function<void(Job &)> work, std::function<void()> notify)
	: m_work(std::move(work))
	, m_notify(std::move(notify))
	{
		try {
			for (auto &worker : m_workers) {
				worker = std::thread([this] { Run(); });
			}
		} catch (...) {
			Stop();
			throw;
		}
	}
	~StorageReadPool() { Stop(); }
	StorageReadPool(const StorageReadPool &) = delete;
	StorageReadPool &operator=(const StorageReadPool &) = delete;

	struct Snapshot
	{
		size_t queued = 0, running = 0, finished = 0;
	};
	Snapshot GetSnapshot() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		Snapshot snapshot;
		for (const auto &slot : m_slots) {
			snapshot.queued += slot.state == State::Queued;
			snapshot.running += slot.state == State::Running;
			snapshot.finished += slot.state == State::Finished;
		}
		return snapshot;
	}

	bool HasCapacity() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		for (const auto &slot : m_slots) {
			if (slot.state == State::Free) {
				return !m_stopping;
			}
		}
		return false;
	}

	// The coordinator can reject a busy volume before allocating a request or
	// touching its file-handle cache. Only Submit transfers ownership.
	bool CanSubmit(const std::string &storage) const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		bool available = false;
		for (const auto &slot : m_slots) {
			if (slot.state != State::Free && slot.storage == storage) {
				return false;
			}
			available |= slot.state == State::Free;
		}
		return available && !m_stopping;
	}

	// Transfers ownership only on success. At most one queued, running OR completed
	// job per storage key is admitted, regardless of how many clients request it.
	bool Submit(const std::string &storage, std::unique_ptr<Job> &job)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_stopping) {
			return false;
		}
		Slot *free = nullptr;
		for (auto &slot : m_slots) {
			if (slot.state != State::Free && slot.storage == storage) {
				return false;
			}
			if (slot.state == State::Free) {
				free = &slot;
			}
		}
		if (free == nullptr) {
			return false;
		}
		free->storage = storage;
		free->job = std::move(job);
		free->sequence = ++m_sequence;
		free->state = State::Queued;
		m_ready.notify_one();
		return true;
	}

	Completion TakeCompleted()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		for (auto &slot : m_slots) {
			if (slot.state == State::Finished) {
				Completion result{ std::move(slot.job), std::move(slot.error) };
				slot.storage.clear();
				slot.state = State::Free;
				return result;
			}
		}
		return {};
	}

	// A new discovery snapshot can give an active path a different key (notably
	// unknown -> a discovered volume). Keep its permit under the new identity too.
	// The callback only examines immutable job metadata; it must not perform I/O.
	template <typename StorageForJob> void ReclassifyStorage(StorageForJob storageForJob)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		for (auto &slot : m_slots) {
			if (slot.state != State::Free) {
				slot.storage = storageForJob(*slot.job, slot.storage);
			}
		}
	}

	// Drain accepted jobs and join before their owners or the application disappear.
	// A kernel read that cannot be interrupted must still finish; never detach it.
	void Stop()
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_stopping = true;
		}
		m_ready.notify_all();
		for (auto &worker : m_workers) {
			if (worker.joinable()) {
				worker.join();
			}
		}
	}

private:
	enum class State
	{
		Free,
		Queued,
		Running,
		Finished
	};
	struct Slot
	{
		State state = State::Free;
		std::string storage;
		std::unique_ptr<Job> job;
		std::exception_ptr error;
		uint64_t sequence = 0;
	};

	void Run()
	{
		for (;;) {
			Slot *next = nullptr;
			{
				std::unique_lock<std::mutex> lock(m_mutex);
				m_ready.wait(lock, [&] {
					for (auto &slot : m_slots) {
						if (slot.state == State::Queued &&
							(next == nullptr || slot.sequence < next->sequence)) {
							next = &slot;
						}
					}
					return next != nullptr || m_stopping;
				});
				if (next == nullptr) {
					return;
				}
				next->state = State::Running;
			}
			std::exception_ptr error;
			try {
				m_work(*next->job);
			} catch (...) {
				error = std::current_exception();
			}
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				next->error = std::move(error);
				next->state = State::Finished;
			}
			m_notify();
		}
	}

	std::function<void(Job &)> m_work;
	std::function<void()> m_notify;
	mutable std::mutex m_mutex;
	std::condition_variable m_ready;
	std::array<Slot, kWorkers> m_slots;
	std::array<std::thread, kWorkers> m_workers;
	uint64_t m_sequence = 0;
	bool m_stopping = false;
};
#endif
