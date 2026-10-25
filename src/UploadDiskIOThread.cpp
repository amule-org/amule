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

#include "UploadDiskIOThread.h"

#include "updownclient.h"    // Needed for CUpDownClient
#include "UploadQueue.h"     // Needed for CUploadQueue
#include "SharedFileList.h"  // Needed for CSharedFileList
#include "KnownFile.h"       // Needed for CKnownFile
#include "PartFile.h"        // Needed for CPartFile
#include "ClientTCPSocket.h" // Needed for CClientTCPSocket
#include "Packet.h"          // Needed for CPacket
#include "amule.h"           // Needed for theApp
#include "GetTickCount.h"    // Needed for TheTime
#include "Logger.h"
#include "OtherFunctions.h" // Needed for IsUploadCompressible
#include "MD4Hash.h"
#include "UploadBandwidthThrottler.h"
#include "UploadBlockLists.h" // Needed for PushDoneBlock
#include "Statistics.h"       // Needed for theStats
#include "UploadPacketBuilder.h"

#include <algorithm>
#include <exception>
#include <cstring>

#define SLOT_COMPRESSIONCHECK_DATARATE (1024 * 150) // 150 KB/s -- above this we may disable compression
#define BIGBUFFER_MINDATARATE (75 * 1024)           // eMule: BIGBUFFER_MINDATARATE

// Max cached complete-file read handles (OpenFile_Struct::file). Past the cap the least
// recently used idle struct is evicted as a new one is created.
static const size_t MAX_CACHED_OPEN_FILES = 32;

// Close a cached handle after this many idle seconds: an idle handle would block a rename or
// delete on Windows and an unmount everywhere.
static const uint32 MAX_IDLE_OPEN_FILE_SECONDS = 60;

// Use one spelling for admission, outstanding permits and cached handles.
static std::string StoragePath(const CPath &path)
{
	wxString spelled = path.GetRaw();
#ifdef __WINDOWS__
	spelled.Replace("\\", "/");
	spelled = spelled.Lower();
#endif
	return spelled.ToStdString(wxConvUTF8);
}

#ifdef __WINDOWS__
static constexpr bool WINDOWS_STORAGE = true;
#else
static constexpr bool WINDOWS_STORAGE = false;
#endif

// eMule ref: CUploadDiskIOThread::CUploadDiskIOThread()
CUploadDiskIOThread::CUploadDiskIOThread()
: wxThread(wxTHREAD_JOINABLE)
, m_condition(m_mutex)
{
	m_bRun = false;
	m_bNewBlocksPending = false;
	m_bSocketNeedsPending = false;

	wxMutexLocker lock(m_mutex);
	if (Create() == wxTHREAD_NO_ERROR) {
		m_bRun = true;
		m_started = Run() == wxTHREAD_NO_ERROR;
		if (!m_started) {
			m_bRun = false;
		}
	}
}

CUploadDiskIOThread::~CUploadDiskIOThread()
{
	wxASSERT(!m_bRun);
}

// eMule ref: CUploadDiskIOThread::EndThread()
void CUploadDiskIOThread::EndThread()
{
	{
		wxMutexLocker lock(m_mutex);
		m_bRun = false;
		m_condition.Signal();
	}
	if (m_started) {
		Wait();
		m_started = false;
	}
}

// Called by the main thread when new block requests are added for a client. Uses a sticky flag so
// the signal is not lost when the thread is between iterations: wxCondition::Signal() is a pure
// pulse and is dropped if no thread is waiting.
void CUploadDiskIOThread::NewBlockRequestsAvailable()
{
	wxMutexLocker lock(m_mutex);
	m_bNewBlocksPending = true;
	m_condition.Signal();
}

// Called by a sending socket when queued payload falls below its refill target.
void CUploadDiskIOThread::SocketNeedsMoreData()
{
	wxMutexLocker lock(m_mutex);
	m_bSocketNeedsPending = true;
	m_condition.Signal();
}

// eMule ref: CUploadDiskIOThread::RunInternal()
ReadRequest_Struct::~ReadRequest_Struct()
{
	ReleasePartFile();
}

void ReadRequest_Struct::ReleasePartFile()
{
	if (pPartFile != nullptr) {
		--pPartFile->m_pendingUploadReads;
		pPartFile = nullptr;
	}
}

void *CUploadDiskIOThread::Entry()
{
	try {
		m_pool = std::make_unique<StorageReadPool<ReadRequest_Struct>>(
			[](ReadRequest_Struct &req) { req.result = ReadBlock(&req); },
			[this] { NewBlockRequestsAvailable(); });
		while (m_bRun) {
			{
				wxMutexLocker lock(m_mutex);
				m_bNewBlocksPending = false;
				m_bSocketNeedsPending = false;
			}
			DrainCompletions(true);
			const uint64 now = GetTickCount64();
			if ((m_storageRefreshed == 0 || now - m_storageRefreshed >= 5000) &&
				m_pool->CanSubmit("storage-discovery")) {
				// Network providers may block even when resolving a mapped drive.
				// Keep discovery on a bounded worker and use the previous snapshot.
				auto req = std::make_unique<ReadRequest_Struct>();
				req->storageMap = std::make_unique<UploadStorageMap>();
#ifdef __WINDOWS__
				// Publish local identities before consulting any network provider.
				req->discoverNetwork = m_storageMapReady;
#endif
				if (m_pool->Submit("storage-discovery", req)) {
					m_storageRefreshed = now;
				}
			}
			std::vector<uint32> clients;
			{
				wxMutexLocker lock(theApp->uploadqueue->GetUploadingListLock());
				for (const auto &ref : theApp->uploadqueue->GetUploadingList()) {
					clients.push_back(ref.GetClient()->ECID());
				}
			}
			for (auto it = m_retryAfter.begin(); it != m_retryAfter.end();) {
				if (std::find(clients.begin(), clients.end(), it->first) == clients.end() ||
					it->second <= now) {
					it = m_retryAfter.erase(it);
				} else {
					++it;
				}
			}
			for (auto it = m_lastDispatch.begin(); it != m_lastDispatch.end();) {
				if (std::find(clients.begin(), clients.end(), it->first) == clients.end()) {
					it = m_lastDispatch.erase(it);
				} else {
					++it;
				}
			}
			const auto lastDispatch = [this](uint32 id) {
				const auto it = m_lastDispatch.find(id);
				return it == m_lastDispatch.end() ? 0 : it->second;
			};
			std::stable_sort(clients.begin(), clients.end(), [&](uint32 left, uint32 right) {
				return lastDispatch(left) < lastDispatch(right);
			});
			for (uint32 clientId : clients) {
				if (!m_bRun || !m_pool->HasCapacity()) {
					break;
				}
				if (m_pendingClients.count(clientId) || m_retryAfter.count(clientId)) {
					continue;
				}
				auto req = PrepareRead(clientId);
				if (req) {
					m_pendingClients.insert(clientId);
					if (m_pool->Submit(req->storage, req)) {
						m_lastDispatch[clientId] = ++m_dispatchSequence;
					} else {
						m_pendingClients.erase(clientId);
					}
				}
			}
			QueueIdleCloses();
			wxMutexLocker lock(m_mutex);
			if (m_bRun && !m_bNewBlocksPending && !m_bSocketNeedsPending) {
				m_condition.WaitTimeout(500);
			}
		}
	} catch (const std::exception &error) {
		AddDebugLogLineC(logClient, wxString("Upload read coordinator failed: ") + error.what());
		m_bRun = false;
	}
	if (m_pool) {
		m_pool->Stop();
		DrainCompletions(false);
		m_pool.reset();
	}
	// Shutdown only: no shared lock is held, and workers no longer access handles.
	m_listOpenFiles.clear();
	return nullptr;
}

void CUploadDiskIOThread::DrainCompletions(bool deliver)
{
	bool dataQueued = false;
	for (;;) {
		auto completed = m_pool->TakeCompleted();
		if (!completed.job) {
			break;
		}
		if (completed.job->storageMap) {
			if (deliver && !completed.error && completed.job->result == READ_OK) {
				m_storageMap = std::move(*completed.job->storageMap);
				m_storageMapReady = true;
				if (!completed.job->discoverNetwork) {
					m_storageRefreshed =
						0; // follow the local snapshot with mapped drives
				}
				m_storageKeys.clear();
				m_pool->ReclassifyStorage([this](const ReadRequest_Struct &req,
								  const std::string &previous) {
					return req.storageMap ? previous
							      : m_storageMap.Lookup(StoragePath(req.path),
									WINDOWS_STORAGE);
				});
				for (auto &file : m_listOpenFiles) {
					file->storage =
						m_storageMap.Lookup(StoragePath(file->path), WINDOWS_STORAGE);
				}
			}
			continue;
		}
		auto *req = completed.job.release();
		m_pendingClients.erase(req->clientId);
		if (deliver && req->result == READ_DEFERRED) {
			// File completion may briefly park a request. Worker completion wakes must
			// not turn that condition into a tight resubmission loop.
			m_retryAfter[req->clientId] = GetTickCount64() + 500;
		}
		if (completed.error) {
			AddDebugLogLineC(logClient, "Upload read worker failed with an unexpected exception");
		}
		if (!deliver || req->closeOnly) {
			DiscardRequest(req);
		} else if (CommitRead(req, completed.error ? READ_FAILED : req->result)) {
			dataQueued = ReadCompletionRoutine(req) || dataQueued;
		}
	}
	// Publish every ready result before waking the sender. Never wait for an
	// outstanding read: a slow volume must not delay this batch's payload.
	if (dataQueued && theApp->uploadBandwidthThrottler != nullptr) {
		theApp->uploadBandwidthThrottler->NewUploadDataAvailable();
	}
}

void CUploadDiskIOThread::QueueIdleCloses()
{
	const uint64 now = GetTickCount64();
	for (auto it = m_listOpenFiles.end(); it != m_listOpenFiles.begin() && m_pool->HasCapacity();) {
		--it;
		const auto &file = *it;
		if (file.use_count() == 1 &&
			(m_needCacheSlot || now - file->lastRead >= 1000 * MAX_IDLE_OPEN_FILE_SECONDS)) {
			auto req = std::make_unique<ReadRequest_Struct>();
			req->closeOnly = true;
			req->pFileStruct = file;
			req->path = file->path;
			req->storage = file->storage;
			if (m_pool->Submit(req->storage, req)) {
				it = m_listOpenFiles.erase(it);
				m_needCacheSlot = false;
			}
		}
	}
}

// Returns the next block to read for this client, or nullptr if there is none or it is enough
// blocks ahead already.
std::unique_ptr<ReadRequest_Struct> CUploadDiskIOThread::PrepareRead(uint32 clientId)
{
	wxMutexLocker uploadLock(theApp->uploadqueue->GetUploadingListLock());
	CUpDownClient *client = theApp->uploadqueue->FindUploadingClient(clientId);
	if (client == nullptr || client->m_bIOError || client->GetSocket() == nullptr ||
		!client->IsConnected()) {
		return nullptr;
	}
	wxMutexLocker lockBlockLists(client->m_blockListLock);

	// GetQueueSessionPayloadUp() is probably outdated, so also add what the socket reports as
	// sent since the last timer tick. PeekSentPayload() is non-resetting (it does not consume
	// the counter SendBlockData() uses) and is protected by m_sendLocker. Calling it from the
	// disk thread is safe: we hold uploadLock, and disconnect/cleanup needs that lock.
	sint64 nCurQueueSessionPayloadUp = client->m_nCurQueueSessionPayloadUp;
	CClientTCPSocket *pSock = client->GetSocket();
	if (pSock != nullptr)
		nCurQueueSessionPayloadUp += (sint64)pSock->PeekSentPayload();
	sint64 addedPayloadQueueSession = client->m_addedPayloadQueueSession;

	bool bFastUpload = client->GetUploadDatarate() > BIGBUFFER_MINDATARATE;
	// Send-ahead depth: how many blocks this thread primes into a fast slot's async send queue
	// (1 otherwise). This is the upload-side in-flight depth -- the mirror of the leecher's
	// request cap -- so it is bandwidth-delay-product limited: on a high-RTT link a shallow
	// buffer drains before the next refill and caps throughput. eMule's 5 is a low-BDP default;
	// 10 keeps a fast slot fed across moderate WAN RTTs. The cost is a transient ~1.8 MB of
	// send-queue data per active fast slot, self-bounded by the OS TCP send buffer.
	const uint32 nBufferLimit = bFastUpload ? ((10 * EMBLOCKSIZE) + 1) : (EMBLOCKSIZE + 1);
	pSock->SetUploadBufferTarget(nBufferLimit);

	if (client->m_BlockRequests_queue.empty() ||
		(addedPayloadQueueSession > nCurQueueSessionPayloadUp &&
			(uint32)(addedPayloadQueueSession - nCurQueueSessionPayloadUp) >= nBufferLimit)) {
		return nullptr;
	}

	try {
		Requested_Block_Struct *currentblock = client->m_BlockRequests_queue.front();

		if (md4cmp(currentblock->FileID, client->GetUploadFileID().GetHash()) != 0) {
			AddDebugLogLineN(logClient,
				"CUploadDiskIOThread::StartCreateNextBlockPackage: Switched fileid, "
				"waiting for mainthread");
			return nullptr;
		}

		CKnownFile *srcfile = theApp->sharedfiles->GetFileByID(CMD4Hash(currentblock->FileID));
		if (srcfile == nullptr) {
			throw wxString("requested file not found");
		}

		if (currentblock->EndOffset > srcfile->GetFileSize()) {
			throw wxString(CFormat("Asked for data up to %d beyond end of file (%d)") %
				       currentblock->EndOffset % srcfile->GetFileSize());
		} else if (currentblock->StartOffset >= currentblock->EndOffset) {
			throw wxString(CFormat("Asked for invalid block (start %d > end %d)") %
				       currentblock->StartOffset % currentblock->EndOffset);
		}

		uint64 togo = currentblock->EndOffset - currentblock->StartOffset;
		if (togo > EMBLOCKSIZE * 3) {
			throw wxString(CFormat("Client requested too large block (%d > %d)") % togo %
				       (EMBLOCKSIZE * 3));
		}

		CPartFile *partFile = nullptr;
		CPath filePath;
		if (srcfile->IsPartFile()) {
			CPartFile *srcPartFile = static_cast<CPartFile *>(srcfile);
			if (!srcPartFile->IsComplete(
				    currentblock->StartOffset, currentblock->EndOffset - 1)) {
				throw wxString(CFormat("Asked for incomplete block (%d - %d)") %
					       currentblock->StartOffset % (currentblock->EndOffset - 1));
			}
			partFile = srcPartFile;
			filePath = srcPartFile->GetFullName();
		} else {
			filePath = srcfile->GetFilePath().JoinPaths(srcfile->GetFileName());
		}

		const std::string path = StoragePath(filePath);
		auto storage = m_storageKeys.find(path);
		if (storage == m_storageKeys.end()) {
			storage =
				m_storageKeys.emplace(path, m_storageMap.Lookup(path, WINDOWS_STORAGE)).first;
		}
		if (!m_pool->CanSubmit(storage->second)) {
			return nullptr;
		}
		auto req = std::make_unique<ReadRequest_Struct>();
		req->session = client->m_uploadReadSession;
		req->clientId = clientId;
		md4cpy(req->ucMD4FileHash, currentblock->FileID);
		req->uStartOffset = currentblock->StartOffset;
		req->uEndOffset = currentblock->EndOffset;
		req->path = filePath;
		req->storage = storage->second;
		if (partFile != nullptr) {
			// Safe to pin: Delete() takes uploadLock before waiting for these pins.
			++partFile->m_pendingUploadReads;
			req->pPartFile = partFile;
		}
		for (auto it = m_listOpenFiles.begin(); it != m_listOpenFiles.end(); ++it) {
			if ((*it)->path.GetRaw() == req->path.GetRaw() && (*it)->storage == req->storage &&
				md4cmp((*it)->ucMD4FileHash, req->ucMD4FileHash) == 0) {
				req->pFileStruct = *it;
				m_listOpenFiles.splice(m_listOpenFiles.begin(), m_listOpenFiles, it);
				break;
			}
		}
		if (!req->pFileStruct) {
			req->pFileStruct = std::make_shared<OpenFile_Struct>();
			auto &file = *req->pFileStruct;
			md4cpy(file.ucMD4FileHash, req->ucMD4FileHash);
			file.path = req->path;
			file.storage = req->storage;
			file.bCompress = IsUploadCompressible(srcfile->GetFileName());
			if (!req->pPartFile && m_listOpenFiles.size() < MAX_CACHED_OPEN_FILES) {
				file.cached = true;
				m_listOpenFiles.push_front(req->pFileStruct);
			} else if (!req->pPartFile) {
				m_needCacheSlot = true;
			}
		}
		req->pFileStruct->lastRead = GetTickCount64();
		return req;
	} catch (const wxString &DEBUG_ONLY(error)) {
		AddDebugLogLineN(logClient,
			CFormat("CUploadDiskIOThread: error for client '%s': %s") % client->GetUserName() %
				error);
		client->m_bIOError = true;
	}
	return nullptr;
}

// Runs with no locks held, and touches neither the client nor the file object, except the
// pinned part file's ReadData and GetStatus.
CUploadDiskIOThread::ReadResult CUploadDiskIOThread::ReadBlock(ReadRequest_Struct *req)
{
	if (req->storageMap) {
		return req->storageMap->Refresh(req->discoverNetwork) ? READ_OK : READ_FAILED;
	}
	// Pins and uncached handles are released on this worker even when allocation,
	// opening or reading throws; the coordinator never waits for that filesystem.
	struct Cleanup
	{
		ReadRequest_Struct *req;
		~Cleanup() noexcept
		{
			try {
				req->ReleasePartFile();
				// Open can fail, and the stale-handle retry closes before throwing. In both
				// cases this cleanup still owns the uncached entry, but no open handle.
				if ((!req->pFileStruct->cached || req->closeOnly) &&
					req->pFileStruct->file.IsOpened()) {
					req->pFileStruct->file.Close();
				}
			} catch (const CMuleException &error) {
				AddDebugLogLineC(logClient,
					wxString("CUploadDiskIOThread: cleanup failed: ") + error.what());
			} catch (...) {
				AddDebugLogLineC(logClient,
					"CUploadDiskIOThread: cleanup failed with an unexpected exception");
			}
		}
	} cleanup{ req };
	if (req->closeOnly) {
		return READ_OK;
	}
	CFileArea &area = req->area;
	const uint32 togo = static_cast<uint32>(req->uEndOffset - req->uStartOffset);
	ReadResult result = READ_OK;
	try {
		if (req->pPartFile != nullptr) {
			bool handleClosed = false;
			if (!req->pPartFile->ReadData(area, req->uStartOffset, togo, &handleClosed)) {
				// A closed handle means PerformFileComplete got there first: the download
				// finished and the file is on its way to Incoming. That is not this
				// client's fault, and an error would set m_bIOError and have
				// CUploadQueue::Process drop it, undoing the graceful SuspendUpload() that
				// parked it on the waiting list to survive the completion. Defer to the
				// main thread, as the file-id switch does.
				//
				// Only for that specific failure: any other false is a real error and must
				// still be reported as one.
				if (handleClosed && req->pPartFile->GetStatus() == PS_COMPLETING) {
					AddDebugLogLineN(logClient,
						"CUploadDiskIOThread::StartCreateNextBlockPackage:"
						" file completing, waiting for mainthread");
					result = READ_DEFERRED;
				} else {
					AddDebugLogLineN(logClient,
						"CUploadDiskIOThread: Failed to read from requested "
						"partfile");
					result = READ_FAILED;
				}
			}
		} else {
			// A cached handle can go stale (NFS ESTALE, SMB reconnect): reopen it once.
			CFileAutoClose &file = req->pFileStruct->file;
			for (int attempt = 0;; ++attempt) {
				try {
					// shareDelete: a cached handle must not block a rename or delete on
					// Windows.
					if (!file.IsOpened() && !file.Open(req->path, CFile::read, true)) {
						result = READ_OPEN_FAILED;
					} else {
						area.ReadAt(file, req->uStartOffset, togo);
					}
					break;
				} catch (const CIOFailureException &) {
					file.Close();
					if (attempt > 0) {
						throw;
					}
				}
			}
		}
		if (result == READ_OK) {
			// An mmap read only maps the range: fault it in on the worker so filesystem
			// reads and SIGBUS handling cannot stall the coordinator while it builds packets.
			area.Prefault();
			if (area.IsMapped()) {
				// Heap reads already own their buffer. Only mapped reads need a copy so
				// packet construction cannot fault file pages on the coordinator.
				req->data.assign(area.GetBuffer(), area.GetBuffer() + togo);
				// A page can fault again during the copy, after Prefault touched it.
				area.CheckError();
				area.Close();
			}
		}
	} catch (const CIOFailureException &error) {
		AddDebugLogLineC(logClient, "CUploadDiskIOThread: IO failure: " + error.what());
		result = READ_FAILED;
	} catch (const CEOFException &) {
		AddDebugLogLineN(logClient, "CUploadDiskIOThread: EOF reading block");
		result = READ_FAILED;
	}
	if (result != READ_OK) {
		// Failed mapped reads must also unmap on their worker, before handing the
		// request back for validation/discard on the coordinator.
		area.Close();
	}
	return result;
}

// Queues the block for sending and takes it off the client's queue. Returns false, and frees
// req, if the client should get no more blocks this pass.
bool CUploadDiskIOThread::CommitRead(ReadRequest_Struct *req, ReadResult result)
{
	wxMutexLocker uploadLock(theApp->uploadqueue->GetUploadingListLock());
	CUpDownClient *client = theApp->uploadqueue->FindUploadingClient(req->clientId);
	if (client == nullptr || result == READ_DEFERRED) {
		DiscardRequest(req);
		return false;
	}
	wxMutexLocker lockBlockLists(client->m_blockListLock);

	CKnownFile *srcfile = theApp->sharedfiles->GetFileByID(CMD4Hash(req->ucMD4FileHash));

	// The lists are cleared only after the client leaves the upload list, so a client
	// that has left and come back while the read ran may have a different block in front.
	// Only this thread pops the queue, so an unchanged front is the block that was read.
	// Checked before the error below: a stale read must not fail the client's new session.
	Requested_Block_Struct *currentblock =
		client->m_BlockRequests_queue.empty() ? nullptr : client->m_BlockRequests_queue.front();
	if (client->m_uploadReadSession != req->session || currentblock == nullptr ||
		md4cmp(currentblock->FileID, req->ucMD4FileHash) != 0 ||
		md4cmp(currentblock->FileID, client->GetUploadFileID().GetHash()) != 0 ||
		currentblock->StartOffset != req->uStartOffset ||
		currentblock->EndOffset != req->uEndOffset) {
		DiscardRequest(req);
		return false;
	}

	if (result == READ_OPEN_FAILED && srcfile != nullptr) {
		AddLogLineN(CFormat(_("Failed to open file (%s), removing from list of shared files.")) %
			    srcfile->GetFileName());
		theApp->sharedfiles->RemoveFile(srcfile);
	}

	if (result != READ_OK || srcfile == nullptr) {
		client->m_bIOError = true;
		DiscardRequest(req);
		return false;
	}

	// The OpenFile_Struct was already found or created in PrepareRead.

	// Mirrors eMule's SetUploadFileID call in the main thread path.
	client->SetUploadFileID(srcfile);

	uint64 togo = req->uEndOffset - req->uStartOffset;
	client->m_addedPayloadQueueSession += static_cast<sint64>(togo);
	srcfile->statistic.AddTransferred(togo);
	PushDoneBlock(client->m_DoneBlocks_list, currentblock);
	client->m_BlockRequests_queue.pop_front();
	return true;
}

// eMule ref: CUploadDiskIOThread::ReadCompletetionRoutine()
bool CUploadDiskIOThread::ReadCompletionRoutine(ReadRequest_Struct *req)
{
	if (req == nullptr) {
		wxASSERT(false);
		return false;
	}

	bool dataQueued = false;

	// The packets are built without uploadLock. Building allocates and copies the block, and
	// compresses it for clients that take that; an allocation the kernel stalls under memory
	// pressure then held the lock for seconds, while the main thread needs it to add or drop
	// an upload slot.
	bool useCompression = false;
	uint32 dataRate = 0;
	const CClientTCPSocket *packedFor = nullptr;
	{
		wxMutexLocker uploadLock(theApp->uploadqueue->GetUploadingListLock());
		CUpDownClient *client = theApp->uploadqueue->FindUploadingClient(req->clientId);

		if (client == nullptr) {
			AddDebugLogLineN(logClient,
				"CUploadDiskIOThread::ReadCompletionRoutine: Client not found in uploadlist "
				"anymore, discarding block");
		} else if (client->GetSocket() == nullptr || !client->IsConnected()) {
			AddDebugLogLineN(logClient,
				"CUploadDiskIOThread::ReadCompletionRoutine: Client has no connected socket");
			client->m_bIOError = true;
		} else {
			CClientTCPSocket *pSocket = client->GetSocket();
			// Disable compression if the socket is starved and the data rate is high. Once
			// disabled for a client it stays disabled for the session.
			if (!client->m_bDisableCompression && req->pFileStruct->bCompress &&
				client->m_byDataCompVer == 1) {
				if (client->GetUploadDatarate() > SLOT_COMPRESSIONCHECK_DATARATE &&
					!pSocket->HasQueues(true) && !pSocket->IsBusyQuickCheck()) {
					client->m_bDisableCompression = true;
				} else {
					useCompression = true;
				}
			}
			dataRate = client->GetUploadDatarate();
			packedFor = pSocket;
		}
	}

	if (packedFor != nullptr) {
		// CommitRead set the file ID.
		CPacketList packetList;
		const uint8_t *payload = req->data.empty() ? req->area.GetBuffer() : req->data.data();
		if (useCompression) {
			UploadPacketBuilder::Packed(payload,
				req->uStartOffset,
				req->uEndOffset,
				packetList,
				req->pFileStruct->ucMD4FileHash,
				dataRate,
				&theStats::AddUpOverheadFileRequest);
		} else {
			UploadPacketBuilder::Standard(payload,
				req->uStartOffset,
				req->uEndOffset,
				packetList,
				req->pFileStruct->ucMD4FileHash,
				dataRate,
				&theStats::AddUpOverheadFileRequest);
		}

		// Held through SendPacket so a concurrent disconnect cannot free the socket, as in
		// eMule. The block belongs to the connection and upload session it was read for.
		{
			wxMutexLocker uploadLock(theApp->uploadqueue->GetUploadingListLock());
			CUpDownClient *client = theApp->uploadqueue->FindUploadingClient(req->clientId);
			CClientTCPSocket *pSocket = client ? client->GetSocket() : nullptr;
			if (client != nullptr && pSocket == packedFor && client->IsConnected()) {
				// A slot can be reused while packets are built. Keep the read session
				// stable from this final check until every packet reaches the socket.
				wxMutexLocker lockBlockLists(client->m_blockListLock);
				if (client->m_uploadReadSession == req->session) {
					for (const auto &packet : packetList) {
						theStats::AddUploadToSoft(
							client->GetClientSoft(), packet.second);
						pSocket->SendPacket(packet.first, true, false, packet.second);
					}
					packetList.clear();
					dataQueued = true;
				}
			} else {
				AddDebugLogLineN(logClient,
					"CUploadDiskIOThread::ReadCompletionRoutine: Client left or "
					"reconnected while its block was packed, discarding it");
			}
		}
		for (const auto &packet : packetList) {
			delete packet.first;
		}
	}

	delete req;
	return dataQueued;
}

// The request owns its snapshot, buffer and file pin until completion/discard.
void CUploadDiskIOThread::DiscardRequest(ReadRequest_Struct *req)
{
	delete req;
}

// File_checked_for_headers
