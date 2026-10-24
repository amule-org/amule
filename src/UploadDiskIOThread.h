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

#ifndef UPLOADDISKIOTHREAD_H
#define UPLOADDISKIOTHREAD_H

#include <list>
#include <atomic>
#include <memory>
#include <map>
#include <vector>
#include "StorageReadPool.h"
#include "UploadStorageMap.h"
#include "UploadBuffering.h"
#include "UploadReadDiagnostics.h"
#include "UploadPipelinePressure.h"

#include <wx/thread.h>

#include "Types.h"
#include "FileArea.h"      // Needed for CFileArea
#include "FileAutoClose.h" // Needed for CFileAutoClose
#include <common/Path.h>   // Needed for CPath

class CPacket;
class CPartFile;
class CUpDownClient;

// The coordinator owns cache metadata; only a worker with the storage permit
// may open/read/close the handle. A request pins its entry until completion.
struct OpenFile_Struct
{
	uint8 ucMD4FileHash[16];
	CFileAutoClose file;
	CPath path;
	std::string storage;
	uint64 lastRead = 0;
	bool cached = false;
	bool bCompress = false;
};

enum UploadReadResult
{
	READ_OK,
	READ_DEFERRED,
	READ_FAILED,
	READ_OPEN_FAILED
};

struct ReadRequest_Struct
{
	~ReadRequest_Struct();
	void ReleasePartFile();
	std::shared_ptr<OpenFile_Struct> pFileStruct;
	uint32 clientId = 0;
	uint64 session = 0;
	uint8 ucMD4FileHash[16];
	uint64 uStartOffset = 0;
	uint64 uEndOffset = 0;
	CPartFile *pPartFile = nullptr;
	CPath path;
	std::string storage;
	CFileArea area;
	std::vector<uint8> data; // owned copy only when area was mapped
	UploadBufferBudget::Lease bufferReservation;
	UploadReadResult result = READ_FAILED;
	bool closeOnly = false;
	std::unique_ptr<UploadStorageMap> storageMap; // discovery job, without a file/client
	bool discoverNetwork = true;
	UploadReadTiming timing;
};

// Port of eMule's CUploadDiskIOThread (UploadDiskIOThread.h:48-86). Windows primitives replaced
// with wxWidgets equivalents:
//   CWinThread           -> wxThread (joinable)
//   CEvent               -> wxCondition + wxMutex
//   WaitForMultipleObjects -> wxCondition::WaitTimeout()
//   ReadFile(OVERLAPPED) -> bounded blocking workers, coordinated by this thread
//   HANDLE file handles  -> CFileAutoClose cached per file in OpenFile_Struct
//   CCriticalSection     -> wxMutex + wxMutexLocker
class CUploadDiskIOThread : public wxThread
{
public:
	CUploadDiskIOThread();
	~CUploadDiskIOThread();

	void EndThread();                 // eMule ref: UploadDiskIOThread.cpp:77
	void NewBlockRequestsAvailable(); // eMule ref: UploadDiskIOThread.h:55
	void SocketNeedsMoreData();       // eMule ref: UploadDiskIOThread.h:56

private:
	void SamplePipelinePressure();
	void *Entry() override; // replaces RunProc/RunInternal

	using ReadResult = UploadReadResult;
	void DrainCompletions(bool deliver);
	void QueueIdleCloses();
	void ReportDiagnostics(bool final = false);
	std::unique_ptr<ReadRequest_Struct> PrepareRead(uint32 clientId);
	static ReadResult ReadBlock(ReadRequest_Struct *req);
	bool CommitRead(ReadRequest_Struct *req, ReadResult result);
	bool ReadCompletionRoutine(ReadRequest_Struct *req); // true if payload was queued
	void DiscardRequest(ReadRequest_Struct *req);        // frees a req never committed

	bool m_started = false;
	std::atomic<bool> m_bRun{ false }; // eMule ref: m_bRun (line 77)

	wxMutex m_mutex;
	wxCondition m_condition; // replaces m_eventNewBlockRequests + m_eventSocketNeedsData
	// Sticky flags: set by Signal() callers, consumed by Entry() before sleeping.
	// Prevents lost wakeups when signal arrives before WaitTimeout() is entered.
	bool m_bNewBlocksPending;   // set by NewBlockRequestsAvailable()
	bool m_bSocketNeedsPending; // set by SocketNeedsMoreData()

	std::unique_ptr<StorageReadPool<ReadRequest_Struct>> m_pool;
	std::map<uint32, uint64> m_pendingClients;
	std::map<uint32, uint64> m_retryAfter;
	// Oldest successful dispatch wins, independently of activity on other volumes.
	std::map<uint32, uint64> m_lastDispatch;
	uint64 m_dispatchSequence = 0;
	UploadStorageMap m_storageMap;
	uint64 m_storageRefreshed = 0;
	bool m_storageMapReady = false;
	// Resolve each spelled path once per mount-table snapshot, including busy volumes.
	std::map<std::string, std::string> m_storageKeys;
	UploadBufferBudget m_bufferBudget;
	UploadReadDiagnostics m_diagnostics;
	bool m_collectDiagnostics = false;
	uint64 m_lastDiagnostics = 0;
	UploadPipelinePressure m_pipelinePressure;
	// Sampling state is private to the coordinator and only used with FileIO enabled.
	UploadPipelinePressure::Snapshot m_pipelineSnapshot;
	uint64 m_lastPipelineLog = 0;
	uint64 m_lastReadMs = 0, m_lastHandoffMs = 0, m_lastCompletedMs = 0;
	bool m_needCacheSlot = false;
	std::list<std::shared_ptr<OpenFile_Struct>> m_listOpenFiles;
};

#endif
// File_checked_for_headers
