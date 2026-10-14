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

#ifndef PARTMETWRITER_H
#define PARTMETWRITER_H

#include "Types.h"       // Needed for uint8, uint64
#include <common/Path.h> // Needed for CPath

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

//! A serialised .part.met and where it goes.
struct CPartMetSaveRequest
{
	//! Bytes 1-4 hold the .part's mtime, which WritePartMet fills in.
	std::vector<uint8> content;
	CPath metPath;
	CPath tmpPath;
	CPath bakPath;
	CPath partPath;
	//! Where the free space is checked.
	CPath dir;
};

struct CPartMetSaveResult
{
	enum Status
	{
		Saved,
		//! Under 100 KB free: nothing was written.
		LowSpace,
		//! detail says why.
		Failed,
		//! An I/O exception; detail is its message.
		IOFailure,
		//! The .tmp came out empty and was removed; the .part.met is untouched.
		ZeroLength
	};
	Status status = Saved;
	wxString detail;
};

/**
 * Writes the .part.met the way SavePartFile always did: into .part.met.tmp, then the old
 * .part.met becomes .part.met.bak and the .tmp is renamed over it. Patches the .part's mtime into
 * @a req's content first.
 */
CPartMetSaveResult WritePartMet(CPartMetSaveRequest &req);

//! One part file's saves, shared between its CPartFile and the writer, so either can go first.
class CPartMetSaveSlot
{
public:
	//! True if a save of @a gen is queued or being written.
	bool Covers(uint64 gen);

	//! Drops a save not yet started and waits for one being written. Afterwards no save of
	//! this slot runs until the next Queue. True if it dropped one.
	bool Cancel();

	//! Waits until the save queued, if any, is written.
	void WaitIdle();

	//! The saves finished since the last call, oldest first, with the generation each wrote.
	std::vector<std::pair<uint64, CPartMetSaveResult>> TakeResults();

private:
	friend class CPartMetWriter;

	//! Kept when nothing takes them, so a part file that is never reaped stays bounded.
	static constexpr size_t kMaxResults = 16;

	std::mutex m_mutex;
	std::condition_variable m_idle;
	//! In the writer's queue. Cancel leaves it set: the writer skips an empty slot.
	bool m_queued = false;
	bool m_hasRequest = false;
	bool m_inFlight = false;
	CPartMetSaveRequest m_request;
	uint64 m_requestGen = 0;
	uint64 m_inFlightGen = 0;
	std::deque<std::pair<uint64, CPartMetSaveResult>> m_results;
};

/**
 * Writes part files' .part.met on its own thread. On a slow or busy disk each save can take
 * seconds, and the main thread, which also answers EC, used to wait for every one.
 *
 * Saves of the same part file coalesce: a newer one replaces one not yet started, and they are
 * written in the order queued.
 */
class CPartMetWriter
{
public:
	using WriteFn = std::function<CPartMetSaveResult(CPartMetSaveRequest &)>;

	explicit CPartMetWriter(WriteFn write = WritePartMet);
	//! Calls EndThread if it was not called.
	~CPartMetWriter();
	CPartMetWriter(const CPartMetWriter &) = delete;
	CPartMetWriter &operator=(const CPartMetWriter &) = delete;

	//! Writes what is queued, then stops the thread. Later saves must be written inline.
	void EndThread();
	bool IsRunning() const { return m_run; }

	//! Queues @a req as the newest save of @a slot, of change generation @a gen.
	void Queue(const std::shared_ptr<CPartMetSaveSlot> &slot, CPartMetSaveRequest req, uint64 gen);

private:
	void Run();

	WriteFn m_write;
	std::mutex m_mutex;
	std::condition_variable m_wake;
	std::atomic<bool> m_run{ true };
	std::deque<std::shared_ptr<CPartMetSaveSlot>> m_queue;
	std::thread m_thread;
};

#endif // PARTMETWRITER_H
// File_checked_for_headers
