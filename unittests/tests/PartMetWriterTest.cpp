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
#include <filesystem>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#ifdef __WXMSW__
#include <wx/msw/wrapwin.h>
#endif

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
	for (const CPath &path : { Part(), Met(), Tmp(), Bak(), Bak().AppendExt(".tmp") }) {
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
	req.partPath = Part();
	req.dir = CPath(".");
	return req;
}

CPartMetGen Gen(uint64 met, uint64 stats = 0)
{
	return { met, stats };
}

//! The newest pending save's state count, 0 if none.
uint64 PendingMet(CPartMetSaveSlot &slot)
{
	const auto pending = slot.Pending();
	return pending ? pending->met : 0;
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

TEST(WritePartMet, RejectsAnIncompleteHeader)
{
	RemoveAll();
	{
		CFile part(Part(), CFile::write);
	}
	auto req = Request(1);
	req.content.resize(4);
	ASSERT_EQUALS(CPartMetSaveResult::Failed, WritePartMet(req).status);
	ASSERT_FALSE(Tmp().FileExists());
	ASSERT_FALSE(Met().FileExists());
	RemoveAll();
}

// Exercise the production install sequence with real files, injecting only operation failures.
TEST(WritePartMet, BackupFailurePreservesLiveMetadata)
{
	RemoveAll();
	{
		CFile part(Part(), CFile::write);
	}
	auto first = Request(1);
	ASSERT_EQUALS(CPartMetSaveResult::Saved, WritePartMet(first).status);
	{
		CFile tmp(Tmp(), CFile::write);
		tmp.WriteUInt8(2);
	}
	unsigned calls = 0;
	const CPartMetFileOps ops = { [](const CPath &, const wxString &) { return false; },
		[&calls](const CPath &, const CPath &, wxString *) {
			++calls;
			return false;
		} };
	const auto result = InstallPartMet(first, ops);
	ASSERT_EQUALS(CPartMetSaveResult::Failed, result.status);
	ASSERT_EQUALS(wxString("Failed to back up part.met file"), result.detail);
	ASSERT_EQUALS(0u, calls);
	ASSERT_EQUALS(1, ReadAll(Met())[0]);
	ASSERT_EQUALS(2, ReadAll(Tmp())[0]);
	RemoveAll();
}

TEST(WritePartMet, FailedInstallPreservesLiveMetadataAndCanRetry)
{
	RemoveAll();
	{
		CFile part(Part(), CFile::write);
	}
	auto req = Request(1);
	ASSERT_EQUALS(CPartMetSaveResult::Saved, WritePartMet(req).status);
	{
		CFile tmp(Tmp(), CFile::write);
		tmp.WriteUInt8(2);
	}
	const CPartMetFileOps failing = { BackupPartMet,
		[&req](const CPath &src, const CPath &dst, wxString *detail) {
			if (dst.GetRaw() == req.metPath.GetRaw()) {
				*detail = "installation denied";
				return false;
			}
			return CPath::ReplaceEntry(src, dst, detail);
		} };
	const auto failure = InstallPartMet(req, failing);
	ASSERT_EQUALS(CPartMetSaveResult::Failed, failure.status);
	ASSERT_EQUALS(wxString("Failed to install part.met file: installation denied"), failure.detail);
	ASSERT_EQUALS(1, ReadAll(Met())[0]);
	ASSERT_EQUALS(1, ReadAll(Bak())[0]);
	ASSERT_EQUALS(2, ReadAll(Tmp())[0]);
	const CPartMetFileOps working = { BackupPartMet, CPath::ReplaceEntry };
	ASSERT_EQUALS(CPartMetSaveResult::Saved, InstallPartMet(req, working).status);
	ASSERT_EQUALS(2, ReadAll(Met())[0]);
	ASSERT_EQUALS(1, ReadAll(Bak())[0]);
	RemoveAll();
}

TEST(WritePartMet, LiveMetadataRemainsDiscoverableBetweenBackupAndInstall)
{
	RemoveAll();
	{
		CFile part(Part(), CFile::write);
	}
	auto req = Request(1);
	ASSERT_EQUALS(CPartMetSaveResult::Saved, WritePartMet(req).status);
	{
		CFile tmp(Tmp(), CFile::write);
		tmp.WriteUInt8(2);
	}
	bool livePresent = false;
	const CPartMetFileOps ops = { BackupPartMet,
		[&livePresent, &req](const CPath &src, const CPath &dst, wxString *detail) {
			if (dst.GetRaw() == req.metPath.GetRaw()) {
				livePresent = Met().FileExists() && ReadAll(Met())[0] == 1 &&
					      ReadAll(Bak())[0] == 1;
			}
			return CPath::ReplaceEntry(src, dst, detail);
		} };
	ASSERT_EQUALS(CPartMetSaveResult::Saved, InstallPartMet(req, ops).status);
	ASSERT_TRUE(livePresent);
	ASSERT_EQUALS(2, ReadAll(Met())[0]);
	RemoveAll();
}

TEST(WritePartMet, FirstInstallFailureDoesNotReportSaved)
{
	RemoveAll();
	{
		CFile tmp(Tmp(), CFile::write);
		tmp.WriteUInt8(2);
	}
	auto req = Request(2);
	const CPartMetFileOps ops = { BackupPartMet, [](const CPath &, const CPath &, wxString *detail) {
					     *detail = "replacement denied";
					     return false;
				     } };
	ASSERT_EQUALS(CPartMetSaveResult::Failed, InstallPartMet(req, ops).status);
	ASSERT_FALSE(Met().FileExists());
	ASSERT_EQUALS(2, ReadAll(Tmp())[0]);
	RemoveAll();
}

TEST(WritePartMet, FailedNativeReplacementCannotTruncateTheDestination)
{
	RemoveAll();
	{
		CFile file(Met(), CFile::write);
		file.WriteUInt8(7);
	}
	// Directory sources must be rejected, including on Windows where MoveFileExW can
	// replace a regular file with a directory. A copy/delete fallback can also destroy it.
	ASSERT_TRUE(CPath::MakeDir(Tmp()));
	wxString detail;
	const bool replaced = CPath::ReplaceEntry(Tmp(), Met(), &detail);
	const auto data = ReadAll(Met());
	CPath::RemoveDir(Tmp());
	RemoveAll();
	ASSERT_FALSE(replaced);
#ifdef __WXMSW__
	ASSERT_EQUALS(wxString("source is a directory"), detail);
#else
	ASSERT_FALSE(detail.empty());
#endif
	ASSERT_EQUALS(1u, data.size());
	ASSERT_EQUALS(7, data[0]);
}

TEST(WritePartMet, MissingReplacementSourcePreservesTheDestination)
{
	RemoveAll();
	{
		CFile file(Met(), CFile::write);
		file.WriteUInt8(7);
	}
	wxString detail;
	ASSERT_FALSE(CPath::ReplaceEntry(Tmp(), Met(), &detail));
	ASSERT_FALSE(detail.empty());
	ASSERT_EQUALS(7, ReadAll(Met())[0]);
	RemoveAll();
}

TEST(WritePartMet, SuccessfulReplacementClearsErrorDetail)
{
	RemoveAll();
	{
		CFile file(Met(), CFile::write);
		file.WriteUInt8(7);
	}
	{
		CFile file(Tmp(), CFile::write);
		file.WriteUInt8(9);
	}
	wxString detail = "previous error";
	ASSERT_TRUE(CPath::ReplaceEntry(Tmp(), Met(), &detail));
	ASSERT_TRUE(detail.empty());
	ASSERT_FALSE(Tmp().FileExists());
	ASSERT_EQUALS(9, ReadAll(Met())[0]);
	RemoveAll();
}

#ifdef __WXMSW__
TEST(WritePartMet, SharingViolationPreservesBothFilesAndReportsError)
{
	RemoveAll();
	{
		CFile file(Met(), CFile::write);
		file.WriteUInt8(7);
	}
	{
		CFile file(Tmp(), CFile::write);
		file.WriteUInt8(9);
	}
	// Hold the live entry without FILE_SHARE_DELETE through both attempts.
	HANDLE held = ::CreateFileW(Met().GetRaw().wc_str(),
		GENERIC_READ,
		FILE_SHARE_READ,
		nullptr,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		nullptr);
	ASSERT_TRUE(held != INVALID_HANDLE_VALUE);
	wxString detail;
	const auto started = std::chrono::steady_clock::now();
	const bool replaced = CPath::ReplaceEntry(Tmp(), Met(), &detail);
	const auto elapsed = std::chrono::steady_clock::now() - started;
	::CloseHandle(held);
	// Windows sleep intervals are quantized by the system timer. Allow that
	// rounding while checking that the sharing failure took the wait path.
	ASSERT_TRUE(elapsed >= std::chrono::milliseconds(25));
	ASSERT_FALSE(replaced);
	ASSERT_FALSE(detail.empty());
	ASSERT_EQUALS(7, ReadAll(Met())[0]);
	ASSERT_EQUALS(9, ReadAll(Tmp())[0]);
	ASSERT_TRUE(CPath::ReplaceEntry(Tmp(), Met(), &detail));
	ASSERT_TRUE(detail.empty());
	RemoveAll();
}
#endif

TEST(WritePartMet, FailedBackupInstallPreservesBothPreviousVersions)
{
	RemoveAll();
	{
		CFile part(Part(), CFile::write);
	}
	auto first = Request(1);
	auto second = Request(2);
	ASSERT_EQUALS(CPartMetSaveResult::Saved, WritePartMet(first).status);
	ASSERT_EQUALS(CPartMetSaveResult::Saved, WritePartMet(second).status);
	{
		CFile tmp(Tmp(), CFile::write);
		tmp.WriteUInt8(3);
	}
	const CPartMetFileOps ops = { BackupPartMet, [](const CPath &, const CPath &, wxString *detail) {
					     *detail = "replacement denied";
					     return false;
				     } };
	const auto failure = InstallPartMet(second, ops);
	ASSERT_EQUALS(CPartMetSaveResult::Failed, failure.status);
	ASSERT_EQUALS(wxString("Failed to back up part.met file: replacement denied"), failure.detail);
	ASSERT_EQUALS(2, ReadAll(Met())[0]);
	ASSERT_EQUALS(1, ReadAll(Bak())[0]);
	ASSERT_EQUALS(3, ReadAll(Tmp())[0]);
	ASSERT_FALSE(Bak().AppendExt(".tmp").FileExists());
	RemoveAll();
}

#ifndef _WIN32
TEST(WritePartMet, BackupSymlinkCannotTruncateTheLiveMetadata)
{
	RemoveAll();
	{
		CFile part(Part(), CFile::write);
	}
	auto first = Request(1);
	ASSERT_EQUALS(CPartMetSaveResult::Saved, WritePartMet(first).status);
	std::filesystem::create_symlink(Met().GetRaw().ToStdString(), Bak().GetRaw().ToStdString());
	auto second = Request(2);
	ASSERT_EQUALS(CPartMetSaveResult::Saved, WritePartMet(second).status);
	ASSERT_EQUALS(2, ReadAll(Met())[0]);
	ASSERT_EQUALS(1, ReadAll(Bak())[0]);
	ASSERT_FALSE(std::filesystem::is_symlink(Bak().GetRaw().ToStdString()));
	RemoveAll();
}
TEST(WritePartMet, TemporaryLinkCannotOverwriteItsTarget)
{
	for (bool dangling : { false, true }) {
		RemoveAll();
		{
			CFile part(Part(), CFile::write);
		}
		auto first = Request(1);
		ASSERT_EQUALS(CPartMetSaveResult::Saved, WritePartMet(first).status);
		const CPath target = dangling ? Bak() : Met();
		std::filesystem::create_symlink(target.GetRaw().ToStdString(), Tmp().GetRaw().ToStdString());
		auto second = Request(2);
		ASSERT_EQUALS(CPartMetSaveResult::Saved, WritePartMet(second).status);
		ASSERT_EQUALS(2, ReadAll(Met())[0]);
		ASSERT_EQUALS(1, ReadAll(Bak())[0]);
		ASSERT_FALSE(std::filesystem::is_symlink(Met().GetRaw().ToStdString()));
		RemoveAll();
	}
}

TEST(WritePartMet, StagedBackupLinkCannotOverwriteItsTarget)
{
	// Check both a live target and a dangling link: BackupPartMet must receive a
	// fresh staging path instead of following a left-over link from an earlier run.
	for (bool dangling : { false, true }) {
		RemoveAll();
		{
			CFile part(Part(), CFile::write);
		}
		auto first = Request(1);
		ASSERT_EQUALS(CPartMetSaveResult::Saved, WritePartMet(first).status);
		const CPath target = dangling ? Bak() : Met();
		std::filesystem::create_symlink(
			target.GetRaw().ToStdString(), Bak().AppendExt(".tmp").GetRaw().ToStdString());
		auto second = Request(2);
		ASSERT_EQUALS(CPartMetSaveResult::Saved, WritePartMet(second).status);
		ASSERT_EQUALS(2, ReadAll(Met())[0]);
		ASSERT_EQUALS(1, ReadAll(Bak())[0]);
		ASSERT_FALSE(std::filesystem::is_symlink(Bak().GetRaw().ToStdString()));
		ASSERT_FALSE(std::filesystem::exists(Bak().AppendExt(".tmp").GetRaw().ToStdString()));
		RemoveAll();
	}
}
#endif

TEST(WritePartMet, RealBackupFailurePropagatesThroughTheWriter)
{
	RemoveAll();
	{
		CFile part(Part(), CFile::write);
	}
	auto first = Request(1);
	ASSERT_EQUALS(CPartMetSaveResult::Saved, WritePartMet(first).status);
	auto second = Request(2);
	// A directory in the staging location must fail without damaging either file.
	ASSERT_TRUE(CPath::MakeDir(Bak().AppendExt(".tmp")));
	CPartMetWriter writer;
	auto slot = std::make_shared<CPartMetSaveSlot>();
	writer.Queue(slot, second, Gen(2));
	slot->WaitIdle();
	writer.EndThread();
	const auto results = slot->TakeResults();
	ASSERT_EQUALS(1u, results.size());
	ASSERT_EQUALS(CPartMetSaveResult::Failed, results[0].second.status);
	ASSERT_EQUALS(1, ReadAll(Met())[0]);
	ASSERT_EQUALS(2, ReadAll(Tmp())[0]);
	ASSERT_TRUE(CPath::RemoveDir(Bak().AppendExt(".tmp")));
	RemoveAll();
}

DECLARE_SIMPLE(CPartMetWriter);

TEST(CPartMetWriter, ANewerSaveReplacesOneNotStarted)
{
	CGatedWrite gate;
	CPartMetWriter writer(Gated(gate));
	auto slot = std::make_shared<CPartMetSaveSlot>();

	writer.Queue(slot, Request(1), Gen(1));
	gate.WaitEntered(1);
	ASSERT_EQUALS(1u, PendingMet(*slot)); // in flight
	writer.Queue(slot, Request(2), Gen(2));
	writer.Queue(slot, Request(3), Gen(3));
	ASSERT_EQUALS(3u, PendingMet(*slot)); // queued, replacing 2

	gate.Release(2);
	writer.EndThread();
	ASSERT_EQUALS(2u, gate.Written().size());
	ASSERT_EQUALS(1, gate.Written()[0]);
	ASSERT_EQUALS(3, gate.Written()[1]);
	ASSERT_FALSE(slot->Pending().has_value());

	const auto results = slot->TakeResults();
	ASSERT_EQUALS(2u, results.size());
	ASSERT_EQUALS(1u, results[0].first.met);
	ASSERT_EQUALS(3u, results[1].first.met);
	ASSERT_EQUALS(CPartMetSaveResult::Saved, results[1].second.status);
	ASSERT_TRUE(slot->TakeResults().empty());
}

TEST(CPartMetWriter, CancelDropsTheQueuedSaveAndWaitsForTheOneInFlight)
{
	CGatedWrite gate;
	CPartMetWriter writer(Gated(gate));
	auto slot = std::make_shared<CPartMetSaveSlot>();

	writer.Queue(slot, Request(1), Gen(1));
	gate.WaitEntered(1);
	writer.Queue(slot, Request(2), Gen(2));

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

	writer.Queue(slot, Request(1), Gen(1));
	gate.WaitEntered(1);
	writer.Queue(slot, Request(2), Gen(2));
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
		writer.Queue(slots.back(), Request(i), Gen(i));
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
		writer.Queue(slot, Request(7), Gen(1));
	}
	gate.Release();
	writer.EndThread();
	ASSERT_EQUALS(1u, gate.Written().size());
}

DECLARE_SIMPLE(PartMetSaveDue);

TEST(PartMetSaveDue, StatsMovingAfterASaveDoNotKeepTheStateDirty)
{
	// A save of state 1 landed with stats at 5, then uploads moved the stats to 9. The save
	// restarted the heartbeat, so nothing is due until it runs out.
	ASSERT_FALSE(PartMetSaveDue(Gen(1, 9), Gen(1, 5), std::nullopt, false));
	ASSERT_TRUE(PartMetSaveDue(Gen(1, 9), Gen(1, 5), std::nullopt, true));
}

TEST(PartMetSaveDue, AStateChangeIsSavedUnlessASaveCarriesIt)
{
	ASSERT_TRUE(PartMetSaveDue(Gen(2), Gen(1), std::nullopt, false));
	// Carried by the save on its way, even though the stats moved since.
	ASSERT_FALSE(PartMetSaveDue(Gen(2, 7), Gen(1), Gen(2, 3), false));
	// A newer change than the one on its way.
	ASSERT_TRUE(PartMetSaveDue(Gen(3), Gen(1), Gen(2), false));
}

TEST(PartMetSaveDue, StatsWaitForTheHeartbeatAndForNoSaveOnItsWay)
{
	ASSERT_FALSE(PartMetSaveDue(Gen(1, 5), Gen(1, 4), std::nullopt, false));
	ASSERT_TRUE(PartMetSaveDue(Gen(1, 5), Gen(1, 4), std::nullopt, true));
	ASSERT_FALSE(PartMetSaveDue(Gen(1, 5), Gen(1, 4), Gen(1, 4), true));
}

TEST(PartMetSaveDue, NothingIsDueWhenClean)
{
	ASSERT_FALSE(PartMetSaveDue(Gen(1, 4), Gen(1, 4), std::nullopt, true));
}
