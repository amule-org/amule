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

#include "PartMetWriter.h" // Interface declarations

#include <wx/filefn.h>   // Needed for wxCopyFile, wxInvalidOffset
#include <wx/filename.h> // Needed for wxFileName::Exists (including dangling symlinks)

#include "CFile.h"            // Needed for CFile, CIOFailureException
#include <common/MuleDebug.h> // Needed for CMuleException

CPartMetSaveResult WritePartMet(CPartMetSaveRequest &req)
{
	const sint64 free = CPath::GetFreeSpaceAt(req.dir);
	if ((free != wxInvalidOffset) && (free < (100 * 1024))) {
		return { CPartMetSaveResult::LowSpace, wxString() };
	}
	if (!req.partPath.FileExists()) {
		return { CPartMetSaveResult::Failed, ".part file not found" };
	}

	if (req.content.size() < 5) {
		return { CPartMetSaveResult::Failed, "Incomplete part.met header" };
	}
	const time_t modified = CPath::GetModificationTime(req.partPath);
	if (modified == static_cast<time_t>(-1)) {
		return { CPartMetSaveResult::Failed, "Failed to read .part modification time" };
	}

	// Little-endian, as CFileDataIO::WriteUInt32 writes it.
	const uint32 mtime = static_cast<uint32>(modified);
	for (int i = 0; i < 4; ++i) {
		req.content.at(1 + i) = static_cast<uint8>(mtime >> (8 * i));
	}

	// A stale temporary entry can be a symlink or hard link to the live metadata.
	// Unlink it before opening for write, rather than truncating its target.
	if (wxFileName::Exists(req.tmpPath.GetRaw()) && !CPath::RemoveFile(req.tmpPath)) {
		return { CPartMetSaveResult::Failed, "Failed to remove stale part.met.tmp file" };
	}

	CFile file;
	try {
		file.Open(req.tmpPath, CFile::write);
		if (!file.IsOpened()) {
			CPath::RemoveFile(req.tmpPath);
			return { CPartMetSaveResult::Failed, "Failed to open part.met.tmp file" };
		}
		file.Write(req.content.data(), req.content.size());
		if (!file.Close()) {
			CPath::RemoveFile(req.tmpPath);
			return { CPartMetSaveResult::Failed, "Failed to close part.met.tmp file" };
		}
	} catch (const CIOFailureException &e) {
		// Closing may retry a failed buffered write and throw again. Preserve the original
		// failure and still remove the partial temporary file, including on inline saves.
		try {
			if (file.IsOpened()) {
				file.Close();
			}
		} catch (const CIOFailureException &) {
			// Best effort: Close has already released the descriptor.
		}
		CPath::RemoveFile(req.tmpPath);
		return { CPartMetSaveResult::IOFailure, e.what() };
	}

	// No exception, yet an empty file (disk full, quota): keep the existing .part.met.
	const sint64 tmpLength = req.tmpPath.GetFileSize();
	if (tmpLength == wxInvalidOffset || tmpLength == 0) {
		CPath::RemoveFile(req.tmpPath);
		return { CPartMetSaveResult::ZeroLength, wxString() };
	}

	if (static_cast<uint64>(tmpLength) != req.content.size()) {
		CPath::RemoveFile(req.tmpPath);
		return { CPartMetSaveResult::Failed, "Incomplete part.met.tmp file" };
	}
	return InstallPartMet(req, { BackupPartMet, CPath::ReplaceEntry });
}

bool BackupPartMet(const CPath &met, const wxString &appendix)
{
	// No fsync: shutdown saves each download in turn, and the new live file is not synced
	// either.
	return ::wxCopyFile(met.GetRaw(), met.AppendExt(appendix).GetRaw(), true);
}

CPartMetSaveResult InstallPartMet(const CPartMetSaveRequest &req, const CPartMetFileOps &ops)
{
	wxString detail;
	// Startup discovers downloads by their live .part.met names. Moving the old file away
	// before installation can hide a download if the second operation fails or we crash.
	if (req.metPath.FileExists()) {
		// Stage the backup too: copying straight over .bak could truncate an earlier backup,
		// or follow a backup symlink into the live file. Replace its directory entry instead.
		const CPath stagedBackup = req.metPath.AppendExt(".bak.tmp");
		// CopyFile on Windows follows destination symlinks. Unlink an old staging
		// entry first, including dangling links, so it cannot overwrite live data.
		if (wxFileName::Exists(stagedBackup.GetRaw()) && !CPath::RemoveFile(stagedBackup)) {
			return { CPartMetSaveResult::Failed, "Failed to remove stale part.met.bak.tmp file" };
		}
		if (!ops.backup(req.metPath, ".bak.tmp") ||
			!ops.replace(stagedBackup, req.metPath.AppendExt(".bak"), &detail)) {
			if (stagedBackup.FileExists()) {
				CPath::RemoveFile(stagedBackup);
			}
			return { CPartMetSaveResult::Failed,
				"Failed to back up part.met file" +
					(detail.empty() ? wxString() : ": " + detail) };
		}
	}
	if (!ops.replace(req.tmpPath, req.metPath, &detail)) {
		return { CPartMetSaveResult::Failed,
			"Failed to install part.met file" + (detail.empty() ? wxString() : ": " + detail) };
	}
	return { CPartMetSaveResult::Saved, wxString() };
}

bool PartMetSaveDue(const CPartMetGen &current,
	const CPartMetGen &saved,
	const std::optional<CPartMetGen> &pending,
	bool heartbeatDue)
{
	if (current.met != saved.met) {
		return !pending || pending->met != current.met;
	}
	return current.stats != saved.stats && heartbeatDue && !pending;
}

std::optional<CPartMetGen> CPartMetSaveSlot::Pending()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_hasRequest) {
		return m_requestGen;
	}
	if (m_inFlight) {
		return m_inFlightGen;
	}
	return std::nullopt;
}

bool CPartMetSaveSlot::Cancel()
{
	std::unique_lock<std::mutex> lock(m_mutex);
	const bool dropped = m_hasRequest;
	m_hasRequest = false;
	m_request = CPartMetSaveRequest();
	m_idle.notify_all();
	m_idle.wait(lock, [this] { return !m_inFlight; });
	return dropped;
}

void CPartMetSaveSlot::WaitIdle()
{
	std::unique_lock<std::mutex> lock(m_mutex);
	m_idle.wait(lock, [this] { return !m_hasRequest && !m_inFlight; });
}

std::vector<std::pair<CPartMetGen, CPartMetSaveResult>> CPartMetSaveSlot::TakeResults()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	std::vector<std::pair<CPartMetGen, CPartMetSaveResult>> results(m_results.begin(), m_results.end());
	m_results.clear();
	return results;
}

CPartMetWriter::CPartMetWriter(WriteFn write)
: m_write(std::move(write))
, m_thread(&CPartMetWriter::Run, this)
{
}

CPartMetWriter::~CPartMetWriter()
{
	EndThread();
}

void CPartMetWriter::EndThread()
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_run = false;
	}
	m_wake.notify_one();
	if (m_thread.joinable()) {
		m_thread.join();
	}
}

void CPartMetWriter::Queue(
	const std::shared_ptr<CPartMetSaveSlot> &slot, CPartMetSaveRequest req, CPartMetGen gen)
{
	bool enqueue;
	{
		std::lock_guard<std::mutex> lock(slot->m_mutex);
		slot->m_request = std::move(req);
		slot->m_requestGen = gen;
		slot->m_hasRequest = true;
		enqueue = !slot->m_queued;
		slot->m_queued = true;
	}
	if (enqueue) {
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_queue.push_back(slot);
		}
		m_wake.notify_one();
	}
}

void CPartMetWriter::Run()
{
	for (;;) {
		std::shared_ptr<CPartMetSaveSlot> slot;
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			m_wake.wait(lock, [this] { return !m_run || !m_queue.empty(); });
			// Drain the queue before stopping: the saves at shutdown carry the final state.
			if (m_queue.empty()) {
				return;
			}
			slot = std::move(m_queue.front());
			m_queue.pop_front();
		}

		CPartMetSaveRequest req;
		CPartMetGen gen;
		{
			std::lock_guard<std::mutex> lock(slot->m_mutex);
			slot->m_queued = false;
			if (!slot->m_hasRequest) {
				continue; // cancelled
			}
			req = std::move(slot->m_request);
			gen = slot->m_requestGen;
			slot->m_hasRequest = false;
			slot->m_inFlight = true;
			slot->m_inFlightGen = gen;
		}

		CPartMetSaveResult result;
		try {
			result = m_write(req);
		} catch (const CMuleException &e) {
			// An exception escaping the thread would terminate the process.
			result = { CPartMetSaveResult::IOFailure, e.what() };
		} catch (const std::exception &e) {
			result = { CPartMetSaveResult::IOFailure, wxString(e.what()) };
		} catch (const wxString &e) {
			result = { CPartMetSaveResult::Failed, e };
		}

		{
			std::lock_guard<std::mutex> lock(slot->m_mutex);
			slot->m_inFlight = false;
			slot->m_results.emplace_back(gen, std::move(result));
			if (slot->m_results.size() > CPartMetSaveSlot::kMaxResults) {
				slot->m_results.pop_front();
			}
		}
		slot->m_idle.notify_all();
	}
}
// File_checked_for_headers
