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

#ifndef DOWNLOADQUEUE_H
#define DOWNLOADQUEUE_H

#include "MD4Hash.h"         // Needed for CMD4Hash
#include "ObservableQueue.h" // Needed for CObservableQueue
#include "GetTickCount.h"    // Needed for GetTickCount64

#include <atomic> // Needed for std::atomic (m_listGeneration)
#include <deque>
#include <functional> // Needed for std::function (LoadProgressCb)

class CSharedFileList;
class CSearchFile;
class CPartFile;
class CUpDownClient;
class CServer;
class CMemFile;
class CKnownFile;
class CED2KLink;
class CED2KFileLink;
class CED2KServerLink;
class CED2KServerListLink;
class CPath;

namespace Kademlia
{
class CUInt128;
}

/// The download queue: every active download. Should be thread-safe.
class CDownloadQueue : public CObservableQueue<CPartFile *>
{
public:
	CDownloadQueue();

	~CDownloadQueue();

	// Progress hook for the part-file load, mirroring CSharedFileList::ReloadYieldCb. Called
	// once per part file with the index just loaded and the total, which -- unlike the shared-
	// file scan -- is known before the loop starts, the directory being enumerated into a
	// vector first.
	using LoadProgressCb = std::function<void(size_t /*loaded*/, size_t /*total*/)>;

	void LoadMetFiles(const CPath &path, const LoadProgressCb &progressCb = nullptr);

	/// Main worker function.
	void Process();

	/// The file with hash @a filehash, or NULL.
	CPartFile *GetFileByID(const CMD4Hash &filehash) const;

	/// The file at index @a idx in the file-list, or NULL if out of range.
	CPartFile *GetFileByIndex(unsigned int idx) const;

	/// True if the file is already being shared or downloaded. @a requestedName is the name it
	/// was requested under; when that differs from the name already held under the hash, it is
	/// appended to the log line so the message can be matched to the download command.
	bool IsFileExisting(const CMD4Hash &fileid, const wxString &requestedName = wxEmptyString);

	/// True if @a file is on the download queue.
	bool IsPartFile(const CKnownFile *file) const;

	/// Updates the files' download active time.
	void OnConnectionState(bool bConnected);

	/// Starts a download from search result @a toadd in @a category, unless an identical file
	/// is already being downloaded or shared.
	void AddSearchToDownload(CSearchFile *toadd, uint8 category);

	/// Adds existing partfile @a newfile to the queue in @a category, stopped if @a paused.
	void AddDownload(CPartFile *newfile, bool paused, uint8 category);

	/// Removes @a toremove from the queue, adding it to the completed list if @a
	/// keepAsCompleted.
	void RemoveFile(CPartFile *toremove, bool keepAsCompleted = false);

	/// Saves the source-seeds of every file on the queue.
	void SaveSourceSeeds();

	/// Loads the source-seeds of every file on the queue.
	void LoadSourceSeeds();

	/// Adds a potential new source to @a sender, checked against the existing clients: it is
	/// queued as appropriate, or deleted if it duplicates one. @a source may be deleted.
	void CheckAndAddSource(CPartFile *sender, CUpDownClient *source);

	/// Like CheckAndAddSource, but @a source is assumed valid and is not checked for being a
	/// duplicate.
	void CheckAndAddKnownSource(CPartFile *sender, CUpDownClient *source);

	/// Removes @a toremove from both normal source lists, the A4AF lists and the downloadqueue
	/// widget, and resets its requestfile. @a bDoStatsUpdate asks the affected files to update
	/// their statistics; @a updatewindow is unused. True if the source was found and removed.
	bool RemoveSource(CUpDownClient *toremove, bool updatewindow = true, bool bDoStatsUpdate = true);

	/// The queued client with this IP and UDP port, found through the file sources, or NULL.
	CUpDownClient *GetDownloadClientByIP_UDP(uint32 dwIP, uint16 nUDPPort) const;

	/// Queues @a sender for source-requesting from the connected server.
	void SendLocalSrcRequest(CPartFile *sender);

	/// Removes @a pFile from the server-request queue.
	void RemoveLocalServerRequest(CPartFile *pFile);

	/// Resets all queued server-requests.
	void ResetLocalServerRequests();

	/// Starts the next paused file on the queue by priority, honouring categories when the
	/// preference is on.
	void StartNextFile(CPartFile *oldfile);

	/// Resets the category of every file in category @a cat.
	void ResetCatParts(uint8 cat);

	/// Sets the priority of every file in category @a cat.
	void SetCatPrio(uint8 cat, uint8 newprio);

	/// Sets the status of every file in category @a cat.
	void SetCatStatus(uint8 cat, int newstatus);

	/// Current number of queued files.
	uint16 GetFileCount() const;

	/**
	 * Changes whenever a file enters or leaves the list(s) this snapshots.
	 *
	 * A change token, not a count: a reload clears and re-adds, so it advances once per file.
	 * All a caller may conclude is "same value means nothing entered or left". Comparing sizes
	 * would be wrong -- an add and a remove between two polls is a net-zero size change that
	 * still has to reconcile.
	 *
	 * Lets a caller that only needs "did membership change" skip the snapshot. CopyFileList is
	 * O(n) and the EC file-list reconcile runs it on every poll for every connected client,
	 * almost always to find nothing came or went.
	 *
	 * Read it BEFORE CopyFileList, never after. Read first and the value can only be older than
	 * the snapshot; a change racing in between just makes the next poll redo the work. Read
	 * after, and a change landing between the copy and the read is recorded as already seen,
	 * and lost.
	 */
	uint64 GetListGeneration() const { return m_listGeneration.load(std::memory_order_relaxed); }

	void CopyFileList(std::vector<CPartFile *> &out_list, bool includeCompleted = false) const;

	/// Current number of downloading files.
	uint16 GetDownloadingFileCount() const;

	/// Current number of paused files.
	uint16 GetPausedFileCount() const;

	/// Called when a DNS lookup finishes.
	void OnHostnameResolved(uint32 ip);

	/// Adds an ed2k or magnet link to the download queue.
	bool AddLink(const wxString &link, uint8 category = 0);

	/// Batch variant of AddLink. Per-link failures are still logged with the protocol reason,
	/// plus one aggregated dialog at the end of the batch.
	void AddLinks(const wxArrayString &links, uint8 category = 0);

	bool AddED2KLink(const wxString &link, uint8 category = 0);
	bool AddED2KLink(const CED2KLink *link, uint8 category = 0);
	bool AddED2KLink(const CED2KFileLink *link, uint8 category = 0);
	bool AddED2KLink(const CED2KServerLink *link);
	bool AddED2KLink(const CED2KServerListLink *link);

	/// The server currently being queried by UDP packets.
	CServer *GetUDPServer() const;

	/// Sets the server to query over UDP.
	void SetUDPServer(CServer *server);

	/// Stops source-requests from non-connected servers.
	void StopUDPRequests();

	/* Kad Stuff */

	/// Adds a Kad source to a download.
	void KademliaSearchFile(uint32_t searchID,
		const Kademlia::CUInt128 *pcontactID,
		const Kademlia::CUInt128 *pkadID,
		uint8_t type,
		uint32_t ip,
		uint16_t tcp,
		uint16_t udp,
		uint32_t buddyip,
		uint16_t buddyport,
		uint8_t byCryptOptions);

	CPartFile *GetFileByKadFileSearchID(uint32 id) const;

	bool DoKademliaFileRequest();

	void SetLastKademliaFileRequest() { lastkademliafilerequest = ::GetTickCount64(); }

	uint32 GetRareFileThreshold() const { return m_rareFileThreshold; }
	uint32 GetCommonFileThreshold() const { return m_commonFileThreshold; }

	/// Removes files from the list of completed downloads.
	void ClearCompleted(const ListOfUInts32 &ecids);

private:
	/// Initialises a new observer with the current contents of the queue.
	virtual void ObserverAdded(ObserverType *o);

	/// Sorts the filelist so that high-priority files come first.
	void DoSortByPriority();

	/** Checks that there is enough free spaces for temp-files at that specified path. */
	void CheckDiskspace(const CPath &path);

	/// Stops performing UDP requests.
	void DoStopUDPRequests();

	void ProcessLocalRequests();

	bool SendNextUDPPacket();
	int GetMaxFilesPerUDPServerPacket() const;
	bool SendGlobGetSourcesUDPPacket(CMemFile &data);

	void AddToResolve(const CMD4Hash &fileid,
		const wxString &pszHostname,
		uint16 port,
		const wxString &hash,
		uint8 cryptoptions);

	//! The mutex associated with this class, mutable to allow for const functions.
	mutable wxMutex m_mutex;

	uint32 m_datarate;
	uint64 m_lastDiskCheck;
	uint64 m_lastudpsearchtime;
	uint64 m_lastsorttime;
	uint64 m_lastudpstattime;
	uint64 m_nLastED2KLinkCheck;
	uint8 m_cRequestsSentToServer;
	uint64 m_dwNextTCPSrcReq;
	uint8 m_udcounter;
	CServer *m_udpserver;

	/// Stores a source with a dynamic hostname.
	struct Hostname_Entry
	{
		//! The ID of the file the source provides.
		CMD4Hash fileid;
		//! The dynamic hostname.
		wxString strHostname;
		//! The user-port of the source.
		uint16 port;
		//! The hash of the source
		wxString hash;
		//! The cryptoptions for the source
		uint8 cryptoptions;
	};

	std::deque<Hostname_Entry> m_toresolve;

	typedef std::deque<CPartFile *> FileQueue;
	FileQueue m_filelist;
	// See GetListGeneration(). Bumped under m_mutex wherever m_filelist OR m_completedDownloads
	// gains or loses an entry -- CopyFileList draws from both when includeCompleted is set,
	// which is how the EC reconcile calls it.
	std::atomic<uint64> m_listGeneration{ 0 };

	typedef std::list<CPartFile *> FileList;
	FileList m_localServerReqQueue;

	//! List of downloads completed and still on display
	FileList m_completedDownloads;

	//! Observer used to keep track of which servers have yet to be asked for sources
	CQueueObserver<CServer *> m_queueServers;

	//! Observer used to keep track of which file to send UDP requests for
	CQueueObserver<CPartFile *> m_queueFiles;

	/* Kad Stuff */
	uint64 lastkademliafilerequest;

	//! Threshold for rare files, dynamically based on the sources for each.
	uint32 m_rareFileThreshold;

	//! Threshold for common files, dynamically based on the sources for each.
	uint32 m_commonFileThreshold;
};

#endif // DOWNLOADQUEUE_H
// File_checked_for_headers
