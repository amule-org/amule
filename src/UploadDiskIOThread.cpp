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
#include "Logger.h"
#include "OtherFunctions.h" // Needed for IsUploadCompressible
#include "MD4Hash.h"
#include "UploadBandwidthThrottler.h"
#include "UploadBlockLists.h" // Needed for PushDoneBlock
#include "Statistics.h"       // Needed for theStats
#include "UploadPacketBuilder.h"

#include <vector>

#define SLOT_COMPRESSIONCHECK_DATARATE (1024 * 150) // 150 KB/s -- above this we may disable compression
#define MAX_FINISHED_REQUESTS_COMPRESSION 15        // max queued finished reads before disabling compression
#define BIGBUFFER_MINDATARATE (75 * 1024)           // eMule: BIGBUFFER_MINDATARATE

// Max cached complete-file read handles (OpenFile_Struct::file). Past the cap the least
// recently used idle struct is evicted as a new one is created. Fixed constant, no preference plumbing.
static const size_t MAX_CACHED_OPEN_FILES = 32;

// eMule ref: CUploadDiskIOThread::CUploadDiskIOThread()
CUploadDiskIOThread::CUploadDiskIOThread()
: wxThread(wxTHREAD_JOINABLE)
, m_condition(m_mutex)
{
	m_bRun = false;
	m_bSignalThrottler = false;
	m_bNewBlocksPending = false;
	m_bSocketNeedsPending = false;

	wxMutexLocker lock(m_mutex);
	if (Create() == wxTHREAD_NO_ERROR) {
		Run();
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
	Wait(); // join -- replaces m_eventThreadEnded->Lock()
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
void *CUploadDiskIOThread::Entry()
{
	m_bRun = true;

	while (m_bRun) // eMule ref: line 88
	{
		std::vector<uint32> clientIds;
		{
			wxMutexLocker uploadLock(theApp->uploadqueue->GetUploadingListLock());
			const CClientRefList &uploadList = theApp->uploadqueue->GetUploadingList();

			for (CClientRefList::const_iterator it = uploadList.begin(); it != uploadList.end();
				++it) {
				CUpDownClient *client = it->GetClient();
				if (client != NULL && client->GetSocket() != NULL && client->IsConnected()) {
					clientIds.push_back(client->ECID());
				}
			}
		}
		for (uint32 clientId : clientIds) {
			StartCreateNextBlockPackage(clientId);
		}

		// Reads are synchronous, so there is no m_listPendingIO: every completed
		// read is already in m_listFinishedIO.
		while (!m_listFinishedIO.empty()) // eMule ref: line 142
		{
			ReadRequest_Struct *req = m_listFinishedIO.front();
			m_listFinishedIO.pop_front();
			ReadCompletionRoutine(req);
		}

		if (m_bSignalThrottler && theApp->uploadBandwidthThrottler != NULL) {
			theApp->uploadBandwidthThrottler->NewUploadDataAvailable();
			m_bSignalThrottler = false;
		}

		// wxCondition::WaitTimeout(500ms) in place of eMule's WaitForMultipleObjects.
		// Signal() is a pure pulse and is dropped if no thread is blocked in WaitTimeout(),
		// so the callers set the sticky flags (m_bNewBlocksPending, m_bSocketNeedsPending)
		// under m_mutex and we check them before sleeping.
		{
			wxMutexLocker lock(m_mutex);
			if (m_bRun && !m_bNewBlocksPending && !m_bSocketNeedsPending) {
				m_condition.WaitTimeout(500);
			}
			m_bNewBlocksPending = false;
			m_bSocketNeedsPending = false;
		}
	}

	// Cleanup. No overlapped I/O to cancel, so just clear the open files list.
	for (std::list<OpenFile_Struct *>::iterator it = m_listOpenFiles.begin(); it != m_listOpenFiles.end();
		++it) {
		delete *it;
	}
	m_listOpenFiles.clear();

	// Discard any unprocessed finished reads
	for (std::list<ReadRequest_Struct *>::iterator it = m_listFinishedIO.begin();
		it != m_listFinishedIO.end();
		++it) {
		delete *it;
	}
	m_listFinishedIO.clear();

	return NULL;
}

// eMule ref: CUploadDiskIOThread::StartCreateNextBlockPackage()
//
// The main thread takes the uploading-list lock to add or drop an upload slot, and
// m_blockListLock for every block request a client sends, so neither is held across the disk
// read. Each block is prepared under both locks, read with none, then committed under both
// again, after checking the client and its request are still there.
void CUploadDiskIOThread::StartCreateNextBlockPackage(uint32 clientId)
{
	while (ReadRequest_Struct *req = PrepareRead(clientId)) {
		if (!CommitRead(req, ReadBlock(req))) {
			return;
		}
	}
}

// Returns the next block to read for this client, or nullptr if there is none or it is enough
// blocks ahead already.
ReadRequest_Struct *CUploadDiskIOThread::PrepareRead(uint32 clientId)
{
	wxMutexLocker uploadLock(theApp->uploadqueue->GetUploadingListLock());
	CUpDownClient *client = theApp->uploadqueue->FindUploadingClient(clientId);
	if (client == nullptr || client->GetSocket() == nullptr || !client->IsConnected()) {
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
		} else if (currentblock->StartOffset > currentblock->EndOffset) {
			throw wxString(CFormat("Asked for invalid block (start %d > end %d)") %
				       currentblock->StartOffset % currentblock->EndOffset);
		}

		uint64 togo = currentblock->EndOffset - currentblock->StartOffset;
		if (togo > EMBLOCKSIZE * 3) {
			throw wxString(CFormat("Client requested too large block (%d > %d)") % togo %
				       (EMBLOCKSIZE * 3));
		}

		ReadRequest_Struct *req = new ReadRequest_Struct;
		req->clientId = clientId;
		md4cpy(req->ucMD4FileHash, currentblock->FileID);
		req->uStartOffset = currentblock->StartOffset;
		req->uEndOffset = currentblock->EndOffset;

		if (srcfile->IsPartFile()) {
			CPartFile *srcPartFile = static_cast<CPartFile *>(srcfile);
			if (!srcPartFile->IsComplete(
				    currentblock->StartOffset, currentblock->EndOffset - 1)) {
				delete req;
				throw wxString(CFormat("Asked for incomplete block (%d - %d)") %
					       currentblock->StartOffset % (currentblock->EndOffset - 1));
			}
			// Safe to pin: the client is uploading this file, and Delete() takes
			// uploadLock to drop the file's uploaders before it waits for the pins.
			++srcPartFile->m_pendingUploadReads;
			req->pPartFile = srcPartFile;
		} else {
			req->path = srcfile->GetFilePath().JoinPaths(srcfile->GetFileName());
		}

		// Find or create the file's OpenFile_Struct here, before the read, so its cached
		// handle exists when ReadBlock needs it. Pure metadata.
		// Applies to part-file requests too: ReadCompletionRoutine needs the hash and
		// bCompress regardless; only complete files ever open the cached handle.
		// Placed after every throw/early-return above, so no refcount can leak.
		OpenFile_Struct *pFileStruct = nullptr;
		for (std::list<OpenFile_Struct *>::iterator it = m_listOpenFiles.begin(); it != m_listOpenFiles.end(); ++it) {
			if (md4cmp((*it)->ucMD4FileHash, req->ucMD4FileHash) == 0) {
				pFileStruct = *it;
				// move it to the 1st position, the most recently used
				m_listOpenFiles.splice(m_listOpenFiles.begin(), m_listOpenFiles, it);
				break;
			}
		}
		if (pFileStruct == nullptr) {
			pFileStruct = new OpenFile_Struct;
			md4cpy(pFileStruct->ucMD4FileHash, req->ucMD4FileHash);
			pFileStruct->nInUse = 0;
			pFileStruct->bCompress = IsUploadCompressible(srcfile->GetFileName());
			pFileStruct->uFileSize = (uint64)srcfile->GetFileSize();
			m_listOpenFiles.push_front(pFileStruct);
		}
		pFileStruct->nInUse++;
		req->pFileStruct = pFileStruct;

		// Enforce the handle-cache cap: evict the least recently used idle struct.
		// Deleting the struct closes its cached handle. If every struct is in use, the
		// cap is exceeded temporarily until some read completes.
		while (m_listOpenFiles.size() > MAX_CACHED_OPEN_FILES) {
			bool evicted = false;
			for (std::list<OpenFile_Struct *>::iterator it = m_listOpenFiles.end(); it != m_listOpenFiles.begin(); ) {
				--it;
				if ((*it)->nInUse == 0) {
					delete *it;
					m_listOpenFiles.erase(it);
					evicted = true;
					break;
				}
			}
			if (!evicted) {
				break;
			}
		}
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
	uint32 togo = (uint32)(req->uEndOffset - req->uStartOffset);
	ReadResult result = READ_OK;
	try {
		if (req->pPartFile != nullptr) {
			bool handleClosed = false;
			if (!req->pPartFile->ReadData(req->area, req->uStartOffset, togo, &handleClosed)) {
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
			// Complete file: read through the struct's cached handle, opened lazily on
			// the first block. The handle stays open between blocks, instead of a per-block 
			// open/close. nInUse > 0 pins the struct against eviction while active reads
			OpenFile_Struct *openFile = req->pFileStruct;
			if (!openFile->file.IsOpened() &&
				!openFile->file.Open(req->path, CFile::read)) {
				result = READ_OPEN_FAILED;
			} else {
				req->area.ReadAt(openFile->file, req->uStartOffset, togo);
			}
		}
		if (result == READ_OK) {
			// An mmap read only maps the range: fault it in now, or the disk reads (and any
			// SIGBUS error) happen while the packets are built under uploadLock.
			req->area.Prefault();
			req->area.CheckError();
		}
	} catch (const CIOFailureException &error) {
		AddDebugLogLineC(logClient, "CUploadDiskIOThread: IO failure: " + error.what());
		result = READ_FAILED;
	} catch (const CEOFException &) {
		AddDebugLogLineN(logClient, "CUploadDiskIOThread: EOF reading block");
		result = READ_FAILED;
	}
	if (req->pPartFile != nullptr) {
		--req->pPartFile->m_pendingUploadReads;
		req->pPartFile = nullptr;
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
	if (result == READ_OPEN_FAILED && srcfile != nullptr) {
		AddLogLineN(CFormat(_("Failed to open file (%s), removing from list of shared files.")) %
			    srcfile->GetFileName());
		theApp->sharedfiles->RemoveFile(srcfile);
	}

	// The lists are cleared only after the client leaves the upload list, so a client
	// that has left and come back while the read ran may have a different block in front.
	// Only this thread pops the queue, so an unchanged front is the block that was read.
	// Checked before the error below: a stale read must not fail the client's new session.
	Requested_Block_Struct *currentblock =
		client->m_BlockRequests_queue.empty() ? nullptr : client->m_BlockRequests_queue.front();
	if (currentblock == nullptr || md4cmp(currentblock->FileID, req->ucMD4FileHash) != 0 ||
		md4cmp(currentblock->FileID, client->GetUploadFileID().GetHash()) != 0 ||
		currentblock->StartOffset != req->uStartOffset ||
		currentblock->EndOffset != req->uEndOffset) {
		DiscardRequest(req);
		return false;
	}

	if (result != READ_OK || srcfile == nullptr) {
		client->m_bIOError = true;
		DiscardRequest(req);
		return false;
	}

	// The OpenFile_Struct was already found or created in PrepareRead; eMule does it here.

	// Mirrors eMule's SetUploadFileID call in the main thread path.
	client->SetUploadFileID(srcfile);

	m_listFinishedIO.push_back(req);

	uint64 togo = req->uEndOffset - req->uStartOffset;
	client->m_addedPayloadQueueSession += static_cast<sint64>(togo);
	srcfile->statistic.AddTransferred(togo);
	PushDoneBlock(client->m_DoneBlocks_list, currentblock);
	client->m_BlockRequests_queue.pop_front();
	return true;
}

// eMule ref: CUploadDiskIOThread::ReadCompletetionRoutine()
void CUploadDiskIOThread::ReadCompletionRoutine(ReadRequest_Struct *req)
{
	if (req == NULL) {
		wxASSERT(false);
		return;
	}

	bool bError = false;

	// Check the client is still in the upload list, and hold uploadLock through SendPacket so a
	// concurrent disconnect cannot free the socket; matches eMule's lock scope.
	{
		wxMutexLocker uploadLock(theApp->uploadqueue->GetUploadingListLock());
		CUpDownClient *client = theApp->uploadqueue->FindUploadingClient(req->clientId);

		if (client == nullptr) {
			AddDebugLogLineN(logClient,
				"CUploadDiskIOThread::ReadCompletionRoutine: Client not found in uploadlist "
				"anymore, discarding block");
			bError = true;
		}

		if (!bError) {
			CClientTCPSocket *pSocket = client->GetSocket();

			if (pSocket == NULL || !client->IsConnected()) {
				AddDebugLogLineN(logClient,
					"CUploadDiskIOThread::ReadCompletionRoutine: Client has no connected "
					"socket");
				bError = true;
				client->m_bIOError = true;
			}

			if (!bError) {
				// Disable compression if the socket is starved and the data rate is
				// high. Once disabled for a client it stays disabled for the session.
				bool bUseCompression = false;
				if (!client->m_bDisableCompression && req->pFileStruct->bCompress &&
					client->m_byDataCompVer == 1) {
					if ((sint32)m_listFinishedIO.size() >
							MAX_FINISHED_REQUESTS_COMPRESSION &&
						theStats::GetUploadRate() > SLOT_COMPRESSIONCHECK_DATARATE) {
						client->m_bDisableCompression = true;
					} else if (client->GetUploadDatarate() >
							   SLOT_COMPRESSIONCHECK_DATARATE &&
						   pSocket != NULL && !pSocket->HasQueues(true) &&
						   !pSocket->IsBusyQuickCheck()) {
						client->m_bDisableCompression = true;
					} else {
						bUseCompression = true;
					}
				}

				// Build packets into a local list, then send them out. CommitRead set
				// the file ID.
				CPacketList packetList;
				uint32 data_rate = client->GetUploadDatarate();
				if (bUseCompression) {
					UploadPacketBuilder::Packed(req->area.GetBuffer(),
						req->uStartOffset,
						req->uEndOffset,
						packetList,
						req->pFileStruct->ucMD4FileHash,
						data_rate,
						&theStats::AddUpOverheadFileRequest);
				} else {
					UploadPacketBuilder::Standard(req->area.GetBuffer(),
						req->uStartOffset,
						req->uEndOffset,
						packetList,
						req->pFileStruct->ucMD4FileHash,
						data_rate,
						&theStats::AddUpOverheadFileRequest);
				}

				for (CPacketList::iterator it = packetList.begin(); it != packetList.end();
					++it) {
					theStats::AddUploadToSoft(client->GetClientSoft(), it->second);
					pSocket->SendPacket(it->first, true, false, it->second);
				}

				m_bSignalThrottler = true;
			}
		}
	} // uploadLock released here

	ReleaseOpenFile(req->pFileStruct);
	delete req;
}

// Frees a request that never reached the finished list, releasing the OpenFile_Struct that
// PrepareRead pinned for it.
void CUploadDiskIOThread::DiscardRequest(ReadRequest_Struct *req)
{
	if (req->pFileStruct != nullptr) {
		ReleaseOpenFile(req->pFileStruct);
	}
	delete req;
}

// eMule ref: CUploadDiskIOThread::ReleaseOvOpenFile()
//
// Decrement nInUse only: structs are not erased at nInUse == 0, are kept for the handle cache.
// They are freed by LRU eviction in PrepareRead, or by the shutdown loop in Entry();
// Deleting a struct closes its cached handle.
bool CUploadDiskIOThread::ReleaseOpenFile(OpenFile_Struct *pFileStruct)
{
	for (std::list<OpenFile_Struct *>::iterator it = m_listOpenFiles.begin(); it != m_listOpenFiles.end(); ++it) {
		if (*it == pFileStruct) {
			pFileStruct->nInUse--;
			return true;
		}
	}
	wxASSERT(false);
	return false;
}

// File_checked_for_headers
