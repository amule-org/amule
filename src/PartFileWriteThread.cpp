//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2002-2011 Merkur ( devs@emule-project.net / http://www.emule-project.net )
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
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA
//

#include "PartFileWriteThread.h"

#include "FreeSpaceSamples.h" // Needed for FreeSpaceAllows

#include "PartFile.h" // Needed for CPartFile, PartFileBufferedData
#include "CFile.h"    // Needed for CIOFailureException
#include "Logger.h"
#include <common/Format.h> // Needed for CFormat

#include <map>
#include <set>
#include <wx/filefn.h>
#include <wx/filename.h>
#ifdef __WXMSW__
#include <windows.h>
#endif

namespace
{
// Run provider queries only on the writer. Resolve the actual directory rather
// than its spelling, so symlinks/bind mounts and Windows volume aliases share
// credits. Failed discovery stays in the conservative unknown group.
std::string IdentifyWriteVolume(ToWrite &write)
{
	const CPath &path = write.partPath;
	CPath spacePath = path;
	std::string volume;
#ifdef __WXMSW__
	spacePath = CPath(); // Unknown target space must fail closed.
	HANDLE handle = CreateFileW(path.GetRaw().wc_str(),
		0,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr,
		OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS,
		nullptr);
	if (handle != INVALID_HANDLE_VALUE) {
		BY_HANDLE_FILE_INFORMATION info{};
		if (GetFileInformationByHandle(handle, &info)) {
			volume = "device:" + std::to_string(info.dwVolumeSerialNumber);
			// Resolve file symlinks/reparse points before choosing a directory
			// for GetDiskFreeSpaceEx. This query stays on the worker too.
			std::vector<wchar_t> finalPath(32768);
			const DWORD size = GetFinalPathNameByHandleW(handle,
				finalPath.data(),
				static_cast<DWORD>(finalPath.size()),
				FILE_NAME_NORMALIZED);
			if (size > 0 && size < finalPath.size()) {
				spacePath = CPath(wxFileName(wxString(finalPath.data(), size)).GetPath());
			} else {
				volume.clear();
			}
		}
		CloseHandle(handle);
	}
#else
	wxStructStat info{};
	if (wxStat(path.GetRaw(), &info) == 0) {
		volume = "device:" + std::to_string(static_cast<uint64_t>(info.st_dev));
	}
#endif
	write.session->volume = volume;
	write.session->spacePath = spacePath;
	WriteBufferBudget::Get().Identify(path.GetRaw().ToStdString(wxConvUTF8), volume);
	return volume;
}

// The free-space check FlushBuffer made on the main thread, where a probe on a busy or hung mount
// blocked it: one live probe per directory per batch, less what the batch writes there. Returns
// the files that do not fit.
std::set<const CPartFile *> FilesWithoutSpace(const std::vector<ToWrite> &batch)
{
	std::map<const CPartFile *, uint64> bytes;
	for (const ToWrite &w : batch) {
		if (w.pBuffer == nullptr || w.session->cancelled.load()) {
			continue;
		}
		bytes[w.pFile] += w.pBuffer->end - w.pBuffer->start + 1;
	}

	std::map<std::string, sint64> freeByVolume;
	std::set<const CPartFile *> noSpace;
	for (const ToWrite &w : batch) {
		if (w.pBuffer == nullptr || w.session->cancelled.load()) {
			continue;
		}
		auto file = bytes.find(w.pFile);
		if (file == bytes.end()) {
			continue; // already decided
		}
		const auto &volume = w.session->volume;
		const std::string key = volume.empty() ? w.partPath.GetRaw().ToStdString(wxConvUTF8) : volume;
		auto dir = freeByVolume.find(key);
		if (dir == freeByVolume.end()) {
			dir = freeByVolume
				      .emplace(key,
					      w.session->spacePath.IsOk()
						      ? CPath::GetFreeSpaceAt(w.session->spacePath)
						      : wxInvalidOffset)
				      .first;
		}
		if (FreeSpaceAllows(dir->second, file->second, w.checkSpace, w.minFree)) {
			dir->second -= static_cast<sint64>(file->second);
		} else {
			noSpace.insert(w.pFile);
		}
		bytes.erase(file);
	}
	return noSpace;
}
} // namespace

CPartFileWriteThread::CPartFileWriteThread()
{
	m_pool = std::make_unique<VolumeWritePool<ToWrite>>(
		[](ToWrite &write) {
			return write.session->cancelled.load() ? std::string("unknown")
							       : IdentifyWriteVolume(write);
		},
		[this](std::vector<ToWrite> &batch) { WriteBatch(batch); },
		[](std::vector<ToWrite> &batch, const std::exception_ptr &) {
			for (auto &write : batch) {
				if (write.pFile != nullptr) {
					if (write.pBuffer == nullptr) {
						write.session->preparing = false;
					}
					Finish(write, PB_ERROR);
				}
			}
		},
		[](ToWrite &write) {
			if (write.session->cancelled.load()) {
				Finish(write, PB_READY);
				return true;
			}
			write.session->ready = true;
			if (write.pBuffer == nullptr) {
				Finish(write, PB_READY);
				return true;
			}
			return false;
		});
}

CPartFileWriteThread::~CPartFileWriteThread() = default;

void CPartFileWriteThread::EndThread()
{
	m_running = false;
	m_pool->Stop();
}

bool CPartFileWriteThread::PrepareFile(CPartFile *file)
{
	auto &session = file->m_writeSession;
	if (session->ready) {
		return true;
	}
	// POSIX open/create already sampled fstat for the known file size. Reuse
	// its descriptor identity without issuing another provider call. In
	// particular, four cold files on a hung mount cannot consume all workers
	// merely to rediscover a device that was known when they opened.
	if (!session->preparing) {
		const auto identity = file->m_hpartfile.GetKnownStorageIdentity();
		if (!identity.empty()) {
			session->volume = identity;
			session->spacePath = file->m_PartPath;
			WriteBufferBudget::Get().Identify(
				file->m_PartPath.GetRaw().ToStdString(wxConvUTF8), identity);
			session->ready = true;
			return true;
		}
	}
	if (!session->preparing.exchange(true)) {
		m_sessions[file] = session;
		++file->m_iWrites;
		if (!m_pool->Submit(std::to_string(session->id),
			    ToWrite{ file, nullptr, file->m_PartPath, false, 0, session },
			    "",
			    true)) {
			--file->m_iWrites;
			session->preparing = false;
		}
	}
	return session->ready;
}

bool CPartFileWriteThread::QueueWrite(
	CPartFile *file, PartFileBufferedData *buffer, bool checkSpace, uint64 minFree)
{
	if (!m_running || file->m_writeSession->cancelled.load()) {
		return false;
	}
	m_sessions[file] = file->m_writeSession;
	return m_pool->Submit(std::to_string(file->m_writeSession->id),
		ToWrite{ file, buffer, file->m_PartPath, checkSpace, minFree, file->m_writeSession },
		file->m_writeSession->ready
			? (file->m_writeSession->volume.empty() ? "unknown" : file->m_writeSession->volume)
			: "");
}

void CPartFileWriteThread::Finish(ToWrite &write, uint8 status)
{
	CPartFile *file = write.pFile;
	if (write.pBuffer != nullptr) {
		write.pBuffer->flushed = status;
	}
	write.pFile = nullptr;
	// Publish the buffer result BEFORE releasing the last file pin. The owner
	// may close/free immediately after observing zero; never touch it again.
	--file->m_iWrites;
}

void CPartFileWriteThread::DropReferencesTo(const CKnownFile *file)
{
	const auto found = m_sessions.find(file);
	if (found != m_sessions.end()) {
		if (auto token = found->second.lock()) {
			token->cancelled.store(true);
		}
		m_sessions.erase(found);
	}
	m_pool->CancelQueued(
		[file](const ToWrite &write) { return static_cast<const CKnownFile *>(write.pFile) == file; },
		[](ToWrite &write) { Finish(write, PB_READY); });
}

void CPartFileWriteThread::WriteBatch(std::vector<ToWrite> &batch)
{
	const auto noSpace = FilesWithoutSpace(batch);
	for (auto &write : batch) {
		if (write.session->cancelled.load()) {
			Finish(write, PB_READY);
			continue;
		}
		if (noSpace.count(write.pFile) != 0) {
			Finish(write, PB_NOSPACE);
			continue;
		}
		const uint32 bytes = static_cast<uint32>(write.pBuffer->end - write.pBuffer->start + 1);
		bool ok = true;
		try {
			std::lock_guard<std::mutex> lock(write.pFile->m_hpartfileMutex);
			write.pBuffer->area.FlushAt(write.pFile->m_hpartfile, write.pBuffer->start, bytes);
		} catch (const CIOFailureException &error) {
			AddDebugLogLineC(logPartFile,
				CFormat("Write thread: I/O failure on '%s': %s") %
					write.pFile->GetFileName() % error.what());
			ok = false;
		}
		Finish(write, ok ? PB_WRITTEN : PB_ERROR);
	}
}
// File_checked_for_headers
