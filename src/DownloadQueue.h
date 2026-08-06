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

/**
 * The download queue houses all active downloads.
 *
 *
 * This class should be thread-safe.
 */
class CDownloadQueue : public CObservableQueue<CPartFile *>
{
public:
	/**
	 * Constructor.
	 */
	CDownloadQueue();

	/**
	 * Destructor.
	 */
	~CDownloadQueue();

	/** Loads met-files from the specified directory. */
	// Progress hook for the part-file load, mirroring
	// CSharedFileList::ReloadYieldCb. Called once per part file with the
	// index just loaded and the total, which -- unlike the shared-file
	// scan -- is known before the loop starts, because the directory is
	// enumerated into a vector first.
	using LoadProgressCb = std::function<void(size_t /*loaded*/, size_t /*total*/)>;

	void LoadMetFiles(const CPath &path, const LoadProgressCb &progressCb = nullptr);

	/**
	 * Main worker function.
	 */
	void Process();

	/**
	 * Returns a pointer to the file with the specified hash, or NULL.
	 *
	 * @param filehash The hash to search for.
	 * @return The corresponding file or NULL.
	 */
	CPartFile *GetFileByID(const CMD4Hash &filehash) const;

	/**
	 * Returns the file at the specified position in the file-list, or NULL if invalid.
	 *
	 * @param A valid position in the file-list.
	 * @return A valid pointer or NULL if the index was invalid.
	 */
	CPartFile *GetFileByIndex(unsigned int idx) const;

	/**
	 * Returns true if the file is currently being shared or downloaded
	 *
	 * @param fileid       Hash of the file the caller is trying to add.
	 * @param requestedName Name the file was requested under (search result or
	 *                      ed2k link). Optional: when it differs from the name
	 *                      of the already-present file (same hash, different
	 *                      filename) it is appended to the log line so the
	 *                      message can be correlated with the download command.
	 */
	bool IsFileExisting(const CMD4Hash &fileid, const wxString &requestedName = wxEmptyString);

	/**
	 * Returns true if the specified file is on the download-queue.
	 */
	bool IsPartFile(const CKnownFile *file) const;

	/**
	 * Updates the file's download active time
	 */
	void OnConnectionState(bool bConnected);

	/**
	 * Starts a new download based on the specified search-result.
	 *
	 * @param toadd The search-result to add.
	 * @param category The category to assign to the new download.
	 *
	 * The download will only be started if no identical files are either
	 * being downloaded or shared currently.
	 */
	void AddSearchToDownload(CSearchFile *toadd, uint8 category);

	/**
	 * Adds an existing partfile to the queue.
	 *
	 * @param newfile The file to add.
	 * @param paused If the file should be stopped when added.
	 * @param category The category to assign to the file.
	 */
	void AddDownload(CPartFile *newfile, bool paused, uint8 category);

	/**
	 * Removes the specified file from the queue.
	 *
	 * @param toremove A pointer to the file object to be removed.
	 * @param keepAsCompleted If true add the removed file to the list of completed files.
	 */
	void RemoveFile(CPartFile *toremove, bool keepAsCompleted = false);

	/**
	 * Saves the source-seeds of every file on the queue.
	 */
	void SaveSourceSeeds();

	/**
	 * Loads the source-seeds of every file on the queue.
	 */
	void LoadSourceSeeds();

	/**
	 * Adds a potiential new client to the specified file.
	 *
	 * @param sender The owner of the new source.
	 * @param source The client in question, might be deleted!
	 *
	 * This function will check the new client against the already existing
	 * clients. The source will then be queued as is appropriate, or deleted
	 * if it is duplicate of an existing client.
	 */
	void CheckAndAddSource(CPartFile *sender, CUpDownClient *source);

	/**
	 * This function adds already known source to the specified file.
	 *
	 * @param sender The owner of the new source.
	 * @param source The client in question.
	 *
	 * This function acts like CheckAndAddSource, with the exception that no
	 * checks are made to see if the client is a duplicate. It is assumed that
	 * it is in fact a valid client.
	 */
	void CheckAndAddKnownSource(CPartFile *sender, CUpDownClient *source);

	/**
	 * Removes the specified client completely.
	 *
	 * @param toremove The client to be removed.
	 * @param updatewindow NOT USED!
	 * @param bDoStatsUdpate Specifies if the affected files should update their statistics.
	 * @return True if the sources was found and removed.
	 *
	 * This function will remove the specified source from both normal source
	 * lists, A4AF lists and the downloadqueue-widget. The requestfile of the
	 * source is also reset.
	 */
	bool RemoveSource(CUpDownClient *toremove, bool updatewindow = true, bool bDoStatsUpdate = true);

	/**
	 * Finds the queued client by IP and UDP-port, by looking at file-sources.
	 *
	 * @param dwIP The IP-address of the client.
	 * @param nUDPPort The UDP-port of the client.
	 * @return The matching client or NULL if none was found.
	 */
	CUpDownClient *GetDownloadClientByIP_UDP(uint32 dwIP, uint16 nUDPPort) const;

	/**
	 * Queues the specified file for source-requestion from the connected server.
	 */
	void SendLocalSrcRequest(CPartFile *sender);

	/**
	 * Removes the specified server from the request-queue.
	 */
	void RemoveLocalServerRequest(CPartFile *pFile);

	/**
	 * Resets all queued server-requests.
	 */
	void ResetLocalServerRequests();

	/**
	 * Starts the next paused file on the queue, going after priority.
	 * Also checks for categories if enabled on preferences.
	 */
	void StartNextFile(CPartFile *oldfile);

	/**
	 * Resets the category of all files with the specified category.
	 */
	void ResetCatParts(uint8 cat);

	/**
	 * Sets the priority of all files with the specified category.
	 */
	void SetCatPrio(uint8 cat, uint8 newprio);

	/**
	 * Sets the status of all files with the specified category.
	 */
	void SetCatStatus(uint8 cat, int newstatus);

	/**
	 * Returns the current number of queued files.
	 */
	uint16 GetFileCount() const;

	/**
	 * Makes a copy of the file list.
	 */
	/**
	 * Changes whenever a file enters or leaves the list(s) this snapshots.
	 *
	 * A change token, not a count of changes: a reload clears and re-adds, so
	 * it advances the value once per file in the library. All a caller may
	 * conclude is "same value means nothing entered or left"; the magnitude of
	 * a difference means nothing. Comparing sizes instead would be wrong --
	 * an add and a remove between two polls is a net-zero size change that
	 * still has to reconcile, and two bumps make that visible.
	 *
	 * Lets a caller that only needs to know "did the membership change since
	 * I last looked" skip taking the snapshot at all. `CopyFileList` is O(n)
	 * and the EC file-list reconcile runs it on every poll for every
	 * connected client, almost always to discover that nothing came or went.
	 *
	 * Read it BEFORE calling CopyFileList, never after. Read first, and the
	 * value can only be older than the snapshot that follows -- a change
	 * racing in between makes the next poll redo the work, which is
	 * harmless. Read after, and a change that landed between the copy and
	 * the read would be recorded as already seen, and lost for good.
	 */
	uint64 GetListGeneration() const { return m_listGeneration.load(std::memory_order_relaxed); }

	void CopyFileList(std::vector<CPartFile *> &out_list, bool includeCompleted = false) const;

	/**
	 * Returns the current number of downloading files.
	 */
	uint16 GetDownloadingFileCount() const;

	/**
	 * Returns the current number of paused files.
	 */
	uint16 GetPausedFileCount() const;

	/**
	 * This function is called when a DNS lookup is finished.
	 */
	void OnHostnameResolved(uint32 ip);

	/**
	 * Adds an ed2k or magnet link to download queue.
	 */
	bool AddLink(const wxString &link, uint8 category = 0);

	/**
	 * Batch variant of AddLink. Per-link failures are still logged with the
	 * specific protocol reason; on top of that, a single aggregated dialog
	 * is shown at the end of the batch (one popup for N failed links).
	 */
	void AddLinks(const wxArrayString &links, uint8 category = 0);

	bool AddED2KLink(const wxString &link, uint8 category = 0);
	bool AddED2KLink(const CED2KLink *link, uint8 category = 0);
	bool AddED2KLink(const CED2KFileLink *link, uint8 category = 0);
	bool AddED2KLink(const CED2KServerLink *link);
	bool AddED2KLink(const CED2KServerListLink *link);

	/**
	 * Returns the current server which is beening queried by UDP packets.
	 */
	CServer *GetUDPServer() const;

	/**
	 * Set the server to query through UDP packest.
	 */
	void SetUDPServer(CServer *server);

	/**
	 * Stop the source-requests from non-connected servers.
	 */
	void StopUDPRequests();

	/* Kad Stuff */

	/**
	 * Add a Kad source to a download
	 */
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

	/**
	 * Remove a file from the list of completed downloads.
	 */
	void ClearCompleted(const ListOfUInts32 &ecids);

private:
	/**
	 * This function initializes new observers with the current contents of the queue.
	 */
	virtual void ObserverAdded(ObserverType *o);

	/**
	 * Helper-function, sorts the filelist so that high-priority files are first.
	 */
	void DoSortByPriority();

	/** Checks that there is enough free spaces for temp-files at that specified path. */
	void CheckDiskspace(const CPath &path);

	/**
	 * Stops performing UDP requests.
	 */
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

	/**
	 * Structure used to store sources with dynamic hostnames.
	 */
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
	// See GetListGeneration(). Bumped under m_mutex wherever m_filelist OR
	// m_completedDownloads gains or loses an entry -- CopyFileList draws
	// from both when includeCompleted is set, which is how the EC reconcile
	// calls it, so tracking only m_filelist would miss completions.
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
