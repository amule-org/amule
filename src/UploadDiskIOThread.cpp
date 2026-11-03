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

void CUploadDiskIOThread::ObserveCoreTick(uint64 now)
{
	if (!theLogger.IsEnabled(logFileIO)) {
		m_lastCoreTick = 0;
		return;
	}
	if (m_lastCoreTick != 0) {
		const uint64 gap = now - m_lastCoreTick;
		m_coreTicks.fetch_add(1, std::memory_order_relaxed);
		if (gap > m_coreTickGapMaximum.load(std::memory_order_relaxed)) {
			m_coreTickGapMaximum.store(gap, std::memory_order_relaxed);
		}
		if (gap > 1000) {
			m_coreTickGapsOverSecond.fetch_add(1, std::memory_order_relaxed);
		}
	}
	m_lastCoreTick = now;
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
			[](ReadRequest_Struct &req) {
				// The coordinator ignores timings from an earlier diagnostics interval.
				if (req.diagnosticEpoch != 0) {
					req.readStarted.store(GetTickCount64(), std::memory_order_relaxed);
				}
				req.result = ReadBlock(&req);
				if (req.diagnosticEpoch != 0) {
					req.readFinished = GetTickCount64();
				}
			},
			[this] { NewBlockRequestsAvailable(); });
	} catch (const std::exception &error) {
		AddLogLineC(CFormat(_("Could not start the upload disk readers: %s")) % error.what());
		m_bRun = false;
		return nullptr;
	}
	uint64 lastErrorLog = 0;
	while (m_bRun) {
		try {
			DispatchReads();
		} catch (const std::exception &error) {
			// Uploads stall silently if this thread stops, so report and keep going.
			const uint64 now = GetTickCount64();
			if (lastErrorLog == 0 || now - lastErrorLog >= 60000) {
				lastErrorLog = now;
				AddLogLineC(CFormat(_("Upload disk reader error: %s")) % error.what());
			}
		}
		wxMutexLocker lock(m_mutex);
		if (m_bRun && !m_bNewBlocksPending && !m_bSocketNeedsPending) {
			m_condition.WaitTimeout(500);
		}
	}
	m_pool->Stop();
	DrainCompletions(false);
	m_pool.reset();
	// Shutdown only: no shared lock is held, and workers no longer access handles.
	m_listOpenFiles.clear();
	return nullptr;
}

void CUploadDiskIOThread::DispatchReads()
{
	{
		wxMutexLocker lock(m_mutex);
		m_bNewBlocksPending = false;
		m_bSocketNeedsPending = false;
	}
	const bool enabled = theLogger.IsEnabled(logFileIO);
	if (enabled != m_diagnostics) {
		m_diagnostics = enabled;
		++m_diagnosticEpoch;
		m_reportStarted = 0;
		m_lastSample = 0;
		m_volumes.clear();
		m_waitingReads.clear();
		m_slotSamples = {};
		m_slotSamplesByClient.clear();
		m_slotSamplePasses = 0;
		// The first report covers its interval, not the time since start.
		if (auto *throttler = theApp->uploadBandwidthThrottler) {
			throttler->TakeAllowedBytes();
			throttler->TakeSpentBytes();
		}
	}
	DrainCompletions(true);
	const uint64 now = GetTickCount64();
	if (m_diagnostics) {
		ObserveDiagnostics(now);
	}
	if ((m_storageRefreshed == 0 || now - m_storageRefreshed >= 5000) &&
		m_pool->CanSubmit("storage-discovery")) {
		// Network providers may block even when resolving a mapped drive.
		// Keep discovery on a bounded worker and use the previous snapshot.
		auto req = std::make_unique<ReadRequest_Struct>();
		req->storageMap = std::make_unique<UploadStorageMap>();
		req->resolveDirectories.assign(m_readDirectories.begin(), m_readDirectories.end());
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
	const auto uploading = [&clients](uint32 id) {
		return std::find(clients.begin(), clients.end(), id) != clients.end();
	};
	for (auto it = m_retryAfter.begin(); it != m_retryAfter.end();) {
		it = !uploading(it->first) || it->second <= now ? m_retryAfter.erase(it) : std::next(it);
	}
	for (auto it = m_lastDispatch.begin(); it != m_lastDispatch.end();) {
		it = !uploading(it->first) ? m_lastDispatch.erase(it) : std::next(it);
	}
	for (auto it = m_clientStorage.begin(); it != m_clientStorage.end();) {
		it = !uploading(it->first) ? m_clientStorage.erase(it) : std::next(it);
	}
	for (auto it = m_turns.begin(); it != m_turns.end();) {
		it = !uploading(it->second.client) ? m_turns.erase(it) : std::next(it);
	}
	const auto lastDispatch = [this](uint32 id) {
		const auto it = m_lastDispatch.find(id);
		return it == m_lastDispatch.end() ? 0 : it->second;
	};
	// A client whose last read ended where its next block starts keeps its volume
	// for a few reads: a disk then reads on without a seek.
	const auto continuing = [this, now](uint32 id) {
		const auto storage = m_clientStorage.find(id);
		if (storage == m_clientStorage.end()) {
			return false;
		}
		const auto turn = m_turns.find(storage->second);
		return turn != m_turns.end() && turn->second.client == id && turn->second.continues &&
		       turn->second.reads < 3 && now - turn->second.started < 50;
	};
	std::stable_sort(clients.begin(), clients.end(), [&](uint32 left, uint32 right) {
		const bool leftContinues = continuing(left);
		if (leftContinues != continuing(right)) {
			return leftContinues;
		}
		return lastDispatch(left) < lastDispatch(right);
	});
	for (uint32 clientId : clients) {
		if (!m_bRun || !m_pool->HasCapacity()) {
			break;
		}
		if (m_pendingClients.count(clientId) || m_retryAfter.count(clientId)) {
			continue;
		}
		// The volume is busy: skip the lookups and checks PrepareRead would repeat.
		const auto known = m_clientStorage.find(clientId);
		if (known != m_clientStorage.end() && !m_pool->CanSubmit(known->second)) {
			if (m_diagnostics) {
				const auto waiting = m_waitingReads.find(clientId);
				// Idle/receiver-limited slots do not need a read from this volume.
				if (waiting != m_waitingReads.end() && waiting->second.active) {
					++m_volumes[known->second].busySkips;
				}
			}
			continue;
		}
		auto req = PrepareRead(clientId);
		if (!req) {
			continue;
		}
		if (m_diagnostics) {
			req->diagnosticEpoch = m_diagnosticEpoch;
		}
		const std::string storage = req->storage;
		if (m_pool->Submit(storage, req)) {
			if (m_diagnostics) {
				const auto waiting = m_waitingReads.find(clientId);
				if (waiting != m_waitingReads.end() && waiting->second.active) {
					m_volumes[storage].dispatch.Add(
						waiting->second.Elapsed(GetTickCount64()));
					waiting->second.Reset();
				}
			}
			m_pendingClients.insert(clientId);
			m_lastDispatch[clientId] = ++m_dispatchSequence;
			auto &turn = m_turns[storage];
			const bool newTurn = turn.client != clientId || !turn.continues;
			if (m_diagnostics) {
				auto &volume = m_volumes[storage];
				// Count a carry-in turn once when an interval begins partway through it.
				if (newTurn || volume.turnReads == 0) {
					++volume.turns;
				}
				++volume.turnReads;
			}
			if (newTurn) {
				turn = Turn{ clientId, 0, now, false };
			}
			++turn.reads;
			turn.continues = false;
		}
	}
	QueueIdleCloses();
	if (m_diagnostics) {
		ReportDiagnostics(now);
	}
}

// All accounting except request timestamps belongs to this coordinator.
void CUploadDiskIOThread::ObserveDiagnostics(uint64 now)
{
	if (m_reportStarted == 0) {
		m_reportStarted = now;
	}
	const bool sample = m_lastSample == 0 || now - m_lastSample >= 1000;
	if (sample) {
		m_lastSample = now;
		++m_slotSamplePasses;
	}
	{
		wxMutexLocker uploadLock(theApp->uploadqueue->GetUploadingListLock());
		for (const auto &ref : theApp->uploadqueue->GetUploadingList()) {
			auto *client = ref.GetClient();
			auto *socket = client->GetSocket();
			const uint32 id = client->ECID();
			if (!socket || !client->IsConnected() || client->m_bIOError) {
				const auto waiting = m_waitingReads.find(id);
				if (waiting != m_waitingReads.end()) {
					waiting->second.Reset();
				}
				continue;
			}
			wxMutexLocker blocks(client->m_blockListLock);
			const auto target = client->GetUploadDatarate() > BIGBUFFER_MINDATARATE
						    ? 10 * EMBLOCKSIZE + 1
						    : EMBLOCKSIZE + 1;
			const auto queued = socket->QueuedUploadPayload();
			const auto state = UploadReadDiagnostics::Classify(m_pendingClients.count(id) != 0,
				!client->m_BlockRequests_queue.empty(),
				queued >= target,
				queued != 0);
			if (sample) {
				++m_slotSamples[static_cast<unsigned>(state)];
				++m_slotSamplesByClient[id][static_cast<unsigned>(state)];
			}
			if (state == UploadReadDiagnostics::SlotState::Storage) {
				const auto *block = client->m_BlockRequests_queue.front();
				m_waitingReads[id].Observe(client->m_uploadReadSession,
					block->StartOffset,
					block->EndOffset,
					block->FileID,
					now);
			} else {
				const auto it = m_waitingReads.find(id);
				if (it != m_waitingReads.end()) {
					it->second.Reset();
				}
			}
		}
		for (auto it = m_waitingReads.begin(); it != m_waitingReads.end();) {
			it = theApp->uploadqueue->FindUploadingClient(it->first) != nullptr
				     ? std::next(it)
				     : m_waitingReads.erase(it);
		}
	}
	// Copy at most four reports out of the pool lock before logging.
	std::vector<wxString> pending;
	m_pool->VisitRunning([&](const ReadRequest_Struct &req) {
		if (req.diagnosticEpoch != m_diagnosticEpoch) {
			return;
		}
		m_volumes.try_emplace(req.storage);
		if (req.pendingReported) {
			return;
		}
		const uint64 started = req.readStarted.load(std::memory_order_relaxed);
		if (started != 0 && now >= started && now - started >= 5000) {
			req.pendingReported = true;
			pending.push_back(CFormat("Upload read pending: volume=%s file=%s elapsed_ms=%u") %
					  wxString::FromUTF8(req.storage.c_str()) % req.path.GetRaw() %
					  (now - started));
		}
	});
	for (const auto &line : pending) {
		AddDebugLogLineN(logFileIO, line);
	}
}

void CUploadDiskIOThread::ReportDiagnostics(uint64 now)
{
	if (now - m_reportStarted < 60000) {
		return;
	}
	for (const auto &entry : m_volumes) {
		const auto &v = entry.second;
		AddDebugLogLineN(logFileIO,
			CFormat("Upload reads: volume=%s reads=%u bytes=%u read_ms_median_upper=%u "
				"read_ms_max=%u "
				"dispatch_ms_median_upper=%u dispatch_ms_max=%u delivery_ms_max=%u turns=%u "
				"reads_per_turn=%s busy_skips=%u") %
				wxString::FromUTF8(entry.first.c_str()) % v.read.count % v.bytes %
				v.read.Median() % v.read.maximum % v.dispatch.Median() % v.dispatch.maximum %
				v.deliveryMaximum % v.turns %
				wxString::Format(
					"%.2f", v.turns ? double(v.turnReads) / double(v.turns) : 0.0) %
				v.busySkips);
	}
	uint64 total = 0;
	for (auto count : m_slotSamples) {
		total += count;
	}
	const auto percent = [&](unsigned i) {
		return total ? 100.0 * double(m_slotSamples[i]) / double(total) : 0.0;
	};
	AddDebugLogLineN(logFileIO,
		CFormat("Upload slots: average=%s receiver-limited=%s%% reading=%s%% "
			"waiting-for-storage=%s%% sending=%s%% waiting-for-peer=%s%% samples=%u") %
			wxString::Format("%.2f",
				m_slotSamplePasses ? double(total) / double(m_slotSamplePasses) : 0.0) %
			wxString::Format("%.1f", percent(2)) % wxString::Format("%.1f", percent(1)) %
			wxString::Format("%.1f", percent(0)) % wxString::Format("%.1f", percent(4)) %
			wxString::Format("%.1f", percent(3)) % total);

	// Built under the lists' locks, logged outside them.
	std::vector<wxString> slots;
	uint64 wakeMaximum = 0;
	{
		using UploadReadDiagnostics::SlotState;
		const auto age = [now](uint64 since) {
			return since != 0 ? wxString::Format("%llu",
						    static_cast<unsigned long long>(
							    UploadReadDiagnostics::ElapsedSince(now, since)))
					  : wxString("none");
		};
		const auto count = [this](uint32 id, SlotState state) {
			const auto samples = m_slotSamplesByClient.find(id);
			return samples != m_slotSamplesByClient.end()
				       ? samples->second[static_cast<unsigned>(state)]
				       : uint64{ 0 };
		};
		wxMutexLocker uploadLock(theApp->uploadqueue->GetUploadingListLock());
		for (const auto &ref : theApp->uploadqueue->GetUploadingList()) {
			auto *client = ref.GetClient();
			auto *socket = client->GetSocket();
			if (!socket || !client->IsConnected() || client->m_bIOError) {
				continue;
			}
			const uint32 id = client->ECID();
			wxMutexLocker blocks(client->m_blockListLock);
			const auto stats = socket->TakeUploadSendStats(now);
			wakeMaximum = std::max<uint64>(wakeMaximum, stats.wakeMaximum);
			slots.push_back(CFormat("Upload slot: id=%u transport=%s storage=%u reading=%u "
						"receiver=%u sending=%u peer=%u rate_kib=%u sent_bytes=%u "
						"queued_bytes=%u unread_requests=%u request_age_ms=%s "
						"send_age_ms=%s refused_writes=%u busy_ms_max=%u "
						"send_wake_ms_max=%u") %
					id % (socket->HasTransport() ? "datagram" : "tcp") %
					count(id, SlotState::Storage) % count(id, SlotState::Reading) %
					count(id, SlotState::Receiver) % count(id, SlotState::Sending) %
					count(id, SlotState::Peer) %
					static_cast<uint64>(client->GetUploadDatarate() / 1024) %
					stats.sentBytes % socket->QueuedUploadPayload() %
					static_cast<uint64>(client->m_BlockRequests_queue.size()) %
					age(client->m_lastBlockRequest) % age(stats.lastSent) %
					stats.refusedWrites % stats.busyMaximum % stats.wakeMaximum);
		}
	}
	for (const auto &line : slots) {
		AddDebugLogLineN(logFileIO, line);
	}
	AddDebugLogLineN(logFileIO,
		CFormat("Main thread: core_ticks=%u gap_ms_max=%u gaps_over_1s=%u send_wake_ms_max=%u") %
			m_coreTicks.exchange(0, std::memory_order_relaxed) %
			m_coreTickGapMaximum.exchange(0, std::memory_order_relaxed) %
			m_coreTickGapsOverSecond.exchange(0, std::memory_order_relaxed) % wakeMaximum);
	if (auto *throttler = theApp->uploadBandwidthThrottler) {
		AddDebugLogLineN(logFileIO,
			CFormat("Upload throttler: allowed_bytes=%u spent_bytes=%u") %
				throttler->TakeAllowedBytes() % throttler->TakeSpentBytes());
	}
	m_volumes.clear();
	m_slotSamples = {};
	m_slotSamplesByClient.clear();
	m_slotSamplePasses = 0;
	m_reportStarted = now;
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
				m_clientStorage.clear();
				m_turns.clear();
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
		auto &req = completed.job;

		m_pendingClients.erase(req->clientId);
		if (deliver && req->result == READ_DEFERRED) {
			// File completion may briefly park a request. Worker completion wakes must
			// not turn that condition into a tight resubmission loop.
			m_retryAfter[req->clientId] = GetTickCount64() + 500;
		}
		if (req->result == READ_OPEN_FAILED && req->pFileStruct) {
			// CommitRead unshares the file; its cache entry must not wait 60 s for a slot.
			m_listOpenFiles.remove(req->pFileStruct);
		}
		if (completed.error) {
			AddDebugLogLineC(logClient, "Upload read worker failed with an unexpected exception");
		}
		if (deliver && !req->closeOnly &&
			CommitRead(*req, completed.error ? READ_FAILED : req->result)) {
			const auto turn = m_turns.find(req->storage);
			if (turn != m_turns.end() && turn->second.client == req->clientId) {
				turn->second.continues = req->continues;
			}
			dataQueued = ReadCompletionRoutine(*req) || dataQueued;
		}
		if (m_diagnostics && req->diagnosticEpoch == m_diagnosticEpoch && req->readFinished != 0 &&
			!req->closeOnly) {
			auto &volume = m_volumes[req->storage];
			const uint64 started = req->readStarted.load(std::memory_order_relaxed);
			volume.read.Add(req->readFinished - started);
			if (!completed.error && req->result == READ_OK) {
				volume.bytes += req->uEndOffset - req->uStartOffset;
			}
			// Delivery ends when packet preparation/queueing has finished, not when
			// the coordinator first takes the completion out of the worker pool.
			volume.deliveryMaximum =
				std::max(volume.deliveryMaximum, GetTickCount64() - req->readFinished);
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

	CClientTCPSocket *pSock = client->GetSocket();
	const bool bFastUpload = client->GetUploadDatarate() > BIGBUFFER_MINDATARATE;
	// Preserve the one-or-ten-block target. Only data requested by the peer
	// can be read; this queue covers storage latency, not data in flight.
	const uint32 nBufferLimit = bFastUpload ? ((10 * EMBLOCKSIZE) + 1) : (EMBLOCKSIZE + 1);
	pSock->SetUploadBufferTarget(nBufferLimit);

	if (client->m_BlockRequests_queue.empty() || pSock->QueuedUploadPayload() >= nBufferLimit) {
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
			if (!WINDOWS_STORAGE && m_readDirectories.size() < 4096) {
				m_readDirectories.insert(StoragePath(filePath.GetPath()));
			}
		}
		m_clientStorage[clientId] = storage->second;
		if (!m_pool->CanSubmit(storage->second)) {
			if (m_diagnostics) {
				++m_volumes[storage->second].busySkips;
			}
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
		req->bCompress = IsUploadCompressible(srcfile->GetFileName());
		if (partFile != nullptr) {
			// Safe to pin: Delete() takes uploadLock before waiting for these pins.
			// Part files read through their own handle and need no cache entry.
			++partFile->m_pendingUploadReads;
			req->pPartFile = partFile;
			return req;
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
			if (m_listOpenFiles.size() < MAX_CACHED_OPEN_FILES) {
				file.cached = true;
				m_listOpenFiles.push_front(req->pFileStruct);
			} else {
				m_needCacheSlot = true;
			}
		}
		req->pFileStruct->lastRead = GetTickCount64();
		return req;
	} catch (const wxString &error) {
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
		const bool refreshed = req->storageMap->Refresh(req->discoverNetwork);
		req->storageMap->ResolveDirectories(req->resolveDirectories);
		return refreshed ? READ_OK : READ_FAILED;
	}
	// Pins and uncached handles are released on this worker even when allocation,
	// opening or reading throws; the coordinator never waits for that filesystem.
	struct Cleanup
	{
		ReadRequest_Struct *req;
		~Cleanup() noexcept
		{
			try {
				// An exception can leave a mapped area: it references the part file, so
				// unmap it before the pin goes, and clear its SIGBUS flag so ~CFileArea
				// cannot throw on the coordinator. A heap area still holds the payload.
				if (req->area.IsMapped()) {
					req->area.Close();
					try {
						req->area.CheckError();
					} catch (const CIOFailureException &) {
					}
				}
				req->ReleasePartFile();
				// Open can fail, and the stale-handle retry closes before throwing. In both
				// cases this cleanup still owns the uncached entry, but no open handle.
				const auto &file = req->pFileStruct;
				if (file && (!file->cached || req->closeOnly) && file->file.IsOpened()) {
					file->file.Close();
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

// Takes the block off the client's queue. Returns false if it must not be sent.
bool CUploadDiskIOThread::CommitRead(ReadRequest_Struct &req, ReadResult result)
{
	wxMutexLocker uploadLock(theApp->uploadqueue->GetUploadingListLock());
	CUpDownClient *client = theApp->uploadqueue->FindUploadingClient(req.clientId);
	if (client == nullptr || result == READ_DEFERRED) {
		return false;
	}
	wxMutexLocker lockBlockLists(client->m_blockListLock);

	CKnownFile *srcfile = theApp->sharedfiles->GetFileByID(CMD4Hash(req.ucMD4FileHash));

	// The lists are cleared only after the client leaves the upload list, so a client
	// that has left and come back while the read ran may have a different block in front.
	// Only this thread pops the queue, so an unchanged front is the block that was read.
	// Checked before the error below: a stale read must not fail the client's new session.
	Requested_Block_Struct *currentblock =
		client->m_BlockRequests_queue.empty() ? nullptr : client->m_BlockRequests_queue.front();
	if (client->m_uploadReadSession != req.session || currentblock == nullptr ||
		md4cmp(currentblock->FileID, req.ucMD4FileHash) != 0 ||
		md4cmp(currentblock->FileID, client->GetUploadFileID().GetHash()) != 0 ||
		currentblock->StartOffset != req.uStartOffset || currentblock->EndOffset != req.uEndOffset) {
		return false;
	}

	if (result == READ_OPEN_FAILED && srcfile != nullptr) {
		AddLogLineN(CFormat(_("Failed to open file (%s), removing from list of shared files.")) %
			    srcfile->GetFileName());
		theApp->sharedfiles->RemoveFile(srcfile);
	}

	if (result != READ_OK || srcfile == nullptr) {
		client->m_bIOError = true;
		return false;
	}

	// Mirrors eMule's SetUploadFileID call in the main thread path.
	client->SetUploadFileID(srcfile);

	uint64 togo = req.uEndOffset - req.uStartOffset;
	srcfile->statistic.AddTransferred(togo);
	PushDoneBlock(client->m_DoneBlocks_list, currentblock);
	client->m_BlockRequests_queue.pop_front();
	const Requested_Block_Struct *next =
		client->m_BlockRequests_queue.empty() ? nullptr : client->m_BlockRequests_queue.front();
	req.continues = next != nullptr && md4cmp(next->FileID, req.ucMD4FileHash) == 0 &&
			next->StartOffset == req.uEndOffset;
	return true;
}

// eMule ref: CUploadDiskIOThread::ReadCompletetionRoutine()
bool CUploadDiskIOThread::ReadCompletionRoutine(ReadRequest_Struct &req)
{
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
		CUpDownClient *client = theApp->uploadqueue->FindUploadingClient(req.clientId);

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
			if (!client->m_bDisableCompression && req.bCompress && client->m_byDataCompVer == 1) {
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
		const uint8_t *payload = req.data.empty() ? req.area.GetBuffer() : req.data.data();
		if (useCompression) {
			UploadPacketBuilder::Packed(payload,
				req.uStartOffset,
				req.uEndOffset,
				packetList,
				req.ucMD4FileHash,
				dataRate,
				&theStats::AddUpOverheadFileRequest);
		} else {
			UploadPacketBuilder::Standard(payload,
				req.uStartOffset,
				req.uEndOffset,
				packetList,
				req.ucMD4FileHash,
				dataRate,
				&theStats::AddUpOverheadFileRequest);
		}

		// Held through SendPacket so a concurrent disconnect cannot free the socket, as in
		// eMule. The block belongs to the connection and upload session it was read for.
		{
			wxMutexLocker uploadLock(theApp->uploadqueue->GetUploadingListLock());
			CUpDownClient *client = theApp->uploadqueue->FindUploadingClient(req.clientId);
			CClientTCPSocket *pSocket = client ? client->GetSocket() : nullptr;
			if (client != nullptr && pSocket == packedFor && client->IsConnected()) {
				// A slot can be reused while packets are built. Keep the read session
				// stable from this final check until every packet reaches the socket.
				wxMutexLocker lockBlockLists(client->m_blockListLock);
				if (client->m_uploadReadSession == req.session) {
					for (const auto &packet : packetList) {
						theStats::AddUploadToSoft(
							client->GetClientSoft(), packet.second);
#ifdef AMULE_SESSION_TRAFFIC_STATS
						theStats::AddSessionTrafficUpload(
							client->IsLowIdToLowIdSession(),
							client->IsIPv6Session(),
							packet.second);
#endif
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

	return dataQueued;
}

// File_checked_for_headers
