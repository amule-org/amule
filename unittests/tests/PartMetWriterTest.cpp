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
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
//

#include <muleunit/test.h>
#include "PartMetWriter.h"
#include "CFile.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

using namespace muleunit;

namespace
{
// Built on first use, not at static init, before the test main sets wx up.
CPath Part()
{
	return CPath("PartMetWriterTest.part");
}
CPath Met()
{
	return Part().AppendExt(".met");
}
CPath Tmp()
{
	return Met().AppendExt(".tmp");
}
CPath Bak()
{
	return Met().AppendExt(".bak");
}

void RemoveAll()
{
	for (const CPath &path : { Part(), Met(), Tmp(), Bak() }) {
		if (path.FileExists()) {
			CPath::RemoveFile(path);
		}
	}
}

CPartMetSaveRequest Request(uint8 marker)
{
	CPartMetSaveRequest req;
	req.content = { marker, 0, 0, 0, 0, 0xAB };
	req.metPath = Met();
	req.tmpPath = Tmp();
	req.bakPath = Bak();
	req.partPath = Part();
	req.dir = CPath(".");
	return req;
}

std::vector<uint8> ReadAll(const CPath &path)
{
	CFile file(path, CFile::read);
	std::vector<uint8> data(file.GetLength());
	file.Read(data.data(), data.size());
	return data;
}

//! A write function that blocks each call until Release, and records the markers it wrote.
class CGatedWrite
{
public:
	CPartMetSaveResult operator()(CPartMetSaveRequest &req)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		++m_entered;
		m_changed.notify_all();
		m_changed.wait(lock, [this] { return m_released > 0; });
		--m_released;
		m_written.push_back(req.content.at(0));
		return { CPartMetSaveResult::Saved, wxString() };
	}

	void WaitEntered(int n)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		m_changed.wait(lock, [this, n] { return m_entered >= n; });
	}

	void Release(int n = 1)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_released += n;
		m_changed.notify_all();
	}

	std::vector<uint8> Written()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_written;
	}

private:
	std::mutex m_mutex;
	std::condition_variable m_changed;
	int m_entered = 0;
	int m_released = 0;
	std::vector<uint8> m_written;
};

CPartMetWriter::WriteFn Gated(CGatedWrite &gate)
{
	return [&gate](CPartMetSaveRequest &req) { return gate(req); };
}
} // namespace

DECLARE_SIMPLE(WritePartMet);

TEST(WritePartMet, WritesThenKeepsTheOldOneAsBackup)
{
	RemoveAll();
	{
		CFile part(Part(), CFile::write);
	}
	const uint32 mtime = static_cast<uint32>(CPath::GetModificationTime(Part()));

	CPartMetSaveRequest first = Request(1);
	ASSERT_EQUALS(CPartMetSaveResult::Saved, WritePartMet(first).status);
	std::vector<uint8> met = ReadAll(Met());
	ASSERT_EQUALS(6u, met.size());
	ASSERT_EQUALS(1, met[0]);
	// The .part's mtime, little-endian.
	ASSERT_EQUALS(mtime, met[1] | (met[2] << 8) | (met[3] << 16) | ((uint32)met[4] << 24));
	ASSERT_EQUALS(0xAB, met[5]);
	ASSERT_FALSE(Tmp().FileExists());
	ASSERT_FALSE(Bak().FileExists());

	CPartMetSaveRequest second = Request(2);
	ASSERT_EQUALS(CPartMetSaveResult::Saved, WritePartMet(second).status);
	ASSERT_EQUALS(2, ReadAll(Met())[0]);
	ASSERT_EQUALS(1, ReadAll(Bak())[0]);
	ASSERT_FALSE(Tmp().FileExists());
	RemoveAll();
}

TEST(WritePartMet, WritesNothingWithoutThePart)
{
	RemoveAll();
	CPartMetSaveRequest req = Request(1);
	const CPartMetSaveResult result = WritePartMet(req);
	ASSERT_EQUALS(CPartMetSaveResult::Failed, result.status);
	ASSERT_FALSE(Met().FileExists());
	ASSERT_FALSE(Tmp().FileExists());
}

DECLARE_SIMPLE(CPartMetWriter);

TEST(CPartMetWriter, ANewerSaveReplacesOneNotStarted)
{
	CGatedWrite gate;
	CPartMetWriter writer(Gated(gate));
	auto slot = std::make_shared<CPartMetSaveSlot>();

	writer.Queue(slot, Request(1), 1);
	gate.WaitEntered(1);
	writer.Queue(slot, Request(2), 2);
	writer.Queue(slot, Request(3), 3);
	ASSERT_TRUE(slot->Covers(1));  // in flight
	ASSERT_FALSE(slot->Covers(2)); // replaced
	ASSERT_TRUE(slot->Covers(3));  // queued

	gate.Release(2);
	writer.EndThread();
	ASSERT_EQUALS(2u, gate.Written().size());
	ASSERT_EQUALS(1, gate.Written()[0]);
	ASSERT_EQUALS(3, gate.Written()[1]);
	ASSERT_FALSE(slot->Covers(3));

	const auto results = slot->TakeResults();
	ASSERT_EQUALS(2u, results.size());
	ASSERT_EQUALS(1u, results[0].first);
	ASSERT_EQUALS(3u, results[1].first);
	ASSERT_EQUALS(CPartMetSaveResult::Saved, results[1].second.status);
	ASSERT_TRUE(slot->TakeResults().empty());
}

TEST(CPartMetWriter, CancelDropsTheQueuedSaveAndWaitsForTheOneInFlight)
{
	CGatedWrite gate;
	CPartMetWriter writer(Gated(gate));
	auto slot = std::make_shared<CPartMetSaveSlot>();

	writer.Queue(slot, Request(1), 1);
	gate.WaitEntered(1);
	writer.Queue(slot, Request(2), 2);

	std::thread release([&gate] {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		gate.Release();
	});
	ASSERT_TRUE(slot->Cancel());
	// Cancel returned, so the save in flight is done.
	ASSERT_EQUALS(1u, gate.Written().size());
	release.join();

	ASSERT_FALSE(slot->Cancel());
	writer.EndThread();
	ASSERT_EQUALS(1u, gate.Written().size());
	ASSERT_EQUALS(1u, slot->TakeResults().size());
}

TEST(CPartMetWriter, WaitIdleWaitsForTheQueuedSave)
{
	CGatedWrite gate;
	CPartMetWriter writer(Gated(gate));
	auto slot = std::make_shared<CPartMetSaveSlot>();

	writer.Queue(slot, Request(1), 1);
	gate.WaitEntered(1);
	writer.Queue(slot, Request(2), 2);
	std::thread release([&gate] {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		gate.Release(2);
	});
	slot->WaitIdle();
	ASSERT_EQUALS(2u, gate.Written().size());
	release.join();
	// Nothing queued: returns at once.
	slot->WaitIdle();
	writer.EndThread();
}

TEST(CPartMetWriter, EndThreadWritesEverySlotQueued)
{
	CGatedWrite gate;
	CPartMetWriter writer(Gated(gate));
	std::vector<std::shared_ptr<CPartMetSaveSlot>> slots;
	for (uint8 i = 0; i < 5; ++i) {
		slots.push_back(std::make_shared<CPartMetSaveSlot>());
		writer.Queue(slots.back(), Request(i), i);
	}
	gate.Release(5);
	writer.EndThread();
	ASSERT_FALSE(writer.IsRunning());
	ASSERT_EQUALS(5u, gate.Written().size());
	for (uint8 i = 0; i < 5; ++i) {
		ASSERT_EQUALS(i, gate.Written()[i]);
	}
}

TEST(CPartMetWriter, ASlotOutlivesItsOwner)
{
	CGatedWrite gate;
	CPartMetWriter writer(Gated(gate));
	{
		auto slot = std::make_shared<CPartMetSaveSlot>();
		writer.Queue(slot, Request(7), 1);
	}
	gate.Release();
	writer.EndThread();
	ASSERT_EQUALS(1u, gate.Written().size());
}
