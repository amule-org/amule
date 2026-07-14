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
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
//

#ifndef WEBAPI_ECSERVICE_H
#define WEBAPI_ECSERVICE_H

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <utility>

class CECPacket;

namespace webapi
{

// Serialises all EC roundtrips onto a single dedicated worker thread with a
// bounded FIFO queue.
//
// Why: amuleapi's one EC connection is a strictly serial request/reply
// protocol, and the HTTP request handlers now run on a worker pool (many
// threads). Letting each pool thread do its own blocking roundtrip meant a
// stalled amuled could park an unbounded number of pool threads on the EC
// mutex until the read timeout fired — starving non-EC requests. It also
// left the single socket reachable from several threads, which is how a
// framing desync becomes possible.
//
// This service fixes both: exactly one thread ever touches the socket
// (no desync), and the queue is bounded (backpressure). When the queue is
// full — i.e. a roundtrip is stalled and callers are piling up behind it —
// further submissions return `nullptr` immediately, which every caller
// already treats as `503 ec_unavailable`. So a stalled EC lane sheds load
// instead of exhausting the handler pool.
//
// Lifetime contract: `Submit` borrows the request pointer; callers block on
// the returned future until completion (the per-roundtrip duration is bounded
// by the EC read timeout), so the caller's stack-owned request outlives the
// worker's use of it. The result pointer follows the pre-existing
// SendRecvSerialized contract — the caller owns and deletes it.
class CEcService
{
public:
	// Runs one EC roundtrip (send request, block for reply); returns the
	// reply packet (caller-owned) or nullptr on EC failure. Bound to
	// CaMuleExternalConnector::SendRecvMsg_v2.
	using Roundtrip = std::function<const CECPacket *(const CECPacket *)>;

	explicit CEcService(std::size_t max_depth = 8)
	: m_max_depth(max_depth)
	{
	}

	~CEcService() { Stop(); }

	CEcService(const CEcService &) = delete;
	CEcService &operator=(const CEcService &) = delete;

	// Start the worker. Call once, after the EC connection is established
	// and before any producer (refresher loop / HTTP handlers) can submit.
	void Start(Roundtrip roundtrip)
	{
		m_roundtrip = std::move(roundtrip);
		m_worker = std::thread([this] { WorkerLoop(); });
	}

	// Enqueue a roundtrip and return a future for its reply. Returns a
	// ready future holding nullptr if the queue is full (backpressure) or
	// the service is stopping — callers map nullptr to 503.
	std::future<const CECPacket *> Submit(const CECPacket *request)
	{
		std::promise<const CECPacket *> prom;
		std::future<const CECPacket *> fut = prom.get_future();
		{
			std::lock_guard<std::mutex> lk(m_mu);
			if (m_stop || m_queue.size() >= m_max_depth) {
				prom.set_value(nullptr);
				return fut;
			}
			m_queue.push_back(Job{ request, std::move(prom) });
		}
		m_cv.notify_one();
		return fut;
	}

	// Stop the worker. Idempotent. Fulfils any queued (not-yet-started)
	// jobs with nullptr so their callers unblock, then joins — bounded by
	// the EC read timeout if a roundtrip is in flight.
	void Stop()
	{
		{
			std::lock_guard<std::mutex> lk(m_mu);
			if (m_stop) {
				return;
			}
			m_stop = true;
			while (!m_queue.empty()) {
				m_queue.front().prom.set_value(nullptr);
				m_queue.pop_front();
			}
		}
		m_cv.notify_all();
		if (m_worker.joinable()) {
			m_worker.join();
		}
	}

private:
	struct Job
	{
		const CECPacket *request;
		std::promise<const CECPacket *> prom;
	};

	void WorkerLoop()
	{
		for (;;) {
			Job job;
			{
				std::unique_lock<std::mutex> lk(m_mu);
				m_cv.wait(lk, [this] { return m_stop || !m_queue.empty(); });
				// Stop drains the queue under the lock, so a wake with an
				// empty queue means we're shutting down.
				if (m_queue.empty()) {
					return;
				}
				job = std::move(m_queue.front());
				m_queue.pop_front();
			}
			const CECPacket *resp = nullptr;
			try {
				resp = m_roundtrip(job.request);
			} catch (...) {
				resp = nullptr;
			}
			job.prom.set_value(resp);
		}
	}

	Roundtrip m_roundtrip;
	std::deque<Job> m_queue;
	std::mutex m_mu;
	std::condition_variable m_cv;
	std::thread m_worker;
	bool m_stop = false;
	const std::size_t m_max_depth;
};

} // namespace webapi

#endif // WEBAPI_ECSERVICE_H
