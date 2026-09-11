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

#ifndef SHAREDFILELIST_H
#define SHAREDFILELIST_H

#include "SharedFilesReloadLatch.h" // Needed for CSharedFilesReloadLatch
#include <atomic>                   // Needed for std::atomic (m_listGeneration, reloading)
#include <functional>
#include <list>
#include <map>
#include <set> // Needed for std::set (m_sharedDirKeys)
#include <unordered_map>
#include <wx/arrstr.h> // Needed for wxArrayString
#include <wx/thread.h> // Needed for wxMutex

#include "Types.h" // Needed for uint16 and uint64

struct UnknownFile_Struct;

class CKnownFileList;
class CKnownFile;
class CMemFile;
class CMD4Hash;
class CServer;
class CPublishKeywordList;
class CPath;
class CAICHHash;
class CThreadTask;
class CSharedDirWatcher;

typedef std::map<CMD4Hash, CKnownFile *> CKnownFileMap;
typedef std::map<wxString, CPath> StringPathMap;
typedef std::list<CPath> PathList;

class CSharedFileList
{
public:
	CSharedFileList(CKnownFileList *in_filelist);
	~CSharedFileList();

	// Yield/cancel hook for chunked reloads, called during the walk with the running file
	// count. Returning false aborts the reload and keeps whatever was added. null is a no-op,
	// for callers with no UI to drive.
	using ReloadYieldCb = std::function<bool(size_t /*filesScanned*/)>;

	void Reload();
	// Cancellable + progress-reporting variant. Returns true if the
	// walk completed normally, false if `yieldCb` requested abort.
	bool Reload(ReloadYieldCb yieldCb);

	// Ask for a full reload from the next Process() tick instead of inline. Callers on the core
	// event loop -- every EC request handler, the watcher's dropped-events fallback -- use this
	// to answer immediately rather than block for the whole walk. That walk takes seconds to
	// minutes on a large or network-mounted share, and amuleapi's EC lane is a single
	// serialised worker, so a blocking reload there stalls the refresher and turns unrelated
	// endpoints into 503s.
	//
	// Repeat requests before the tick coalesce; one arriving mid-walk keeps the flag set so it
	// runs afterwards instead of nesting.
	//
	// A plain bool is deliberate: every setter and the reader run on the core event loop. A
	// caller off that thread is the thing to fix -- do not make this atomic.
	void RequestReload() { m_reloadLatch.Request(); }

	// True when a RequestReload() is outstanding. GUI callers run the owed walk themselves
	// behind a progress dialog rather than let it land silently on a tick.
	bool IsReloadPending() const { return m_reloadLatch.IsPending(); }
	void SafeAddKFile(CKnownFile *toadd, bool bOnlyAdd = false);
	void RemoveFile(CKnownFile *toremove);
	CKnownFile *GetFileByID(const CMD4Hash &filehash);
	short GetFilePriorityByID(const CMD4Hash &filehash);
	const CKnownFile *GetFileByIndex(unsigned int index) const;
	size_t GetCount()
	{
		wxMutexLocker lock(list_mut);
		return m_Files_map.size();
	}
	size_t GetFileCount()
	{
		wxMutexLocker lock(list_mut);
		return m_Files_map.size();
	}
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

	void CopyFileList(std::vector<CKnownFile *> &out_list) const;
	// Fill `out` with the basenames of all currently shared files. Used by
	// the Directories panel's exclusion-filter live preview.
	void GetSharedFileNames(wxArrayString &out) const;
	void UpdateItem(CKnownFile *toupdate);
	void GetSharedFilesByDirectory(const wxString &directory, CKnownFilePtrList &list);
	void ClearED2KPublishInfo();
	void RepublishFile(CKnownFile *pFile);
	void Process();
	void PublishNextTurn() { m_lastPublishED2KFlag = true; }
	bool RenameFile(CKnownFile *pFile, const CPath &newName);
	void VerifyLocalData(const CKnownFile *file) const;

	// Re-extract media metadata for every shared file, whether or not it has any. Returns how
	// many probes were queued.
	//
	// The only way to correct wrong metadata rather than missing: the scheduler skips anything
	// already carrying a media tag, so a value from an older build is otherwise permanent short
	// of deleting known.met, which also discards the ed2k part hashes and every per-file
	// statistic.
	//
	// Asynchronous, on the media-probe worker. Nothing else is touched -- the file is not re-
	// hashed and never leaves the share.
	unsigned RefreshAllMediaMetadata();

	// The single-file form, by hash. False when no shared file has that hash, or it is not
	// eligible: not audio/video by extension, or an incomplete download.
	bool RefreshMediaMetadata(const CMD4Hash &hash);

	// The batched form the GUI uses; returns how many probes were queued. Lets amulegui send
	// ONE EC request for a selection: its request fifo stalls the GUI's own polling past about
	// twenty in flight.
	unsigned RefreshMediaMetadata(const std::vector<CMD4Hash> &hashes);

	/**
	 * Returns the name of a shared folder as the public sees it.
	 *
	 * @param dir The full path to a shared directory.
	 * @return The public name, made only of subdirectories that are shared, so the client's own
	 * directory structure stays hidden: /ed2k/shared/games/tetris -> "games/tetris" when
	 * /ed2k/shared is not itself shared.
	 */
	wxString GetPublicSharedDirName(const CPath &dir);
	const CPath *GetDirForPublicSharedDirName(const wxString &strSharedDir) const;

	/// True if the path is a shared directory or a single shared file.
	bool IsShared(const CPath &path) const;

	/* Kad Stuff */
	void Publish();
	void AddKeywords(CKnownFile *pFile);
	void RemoveKeywords(CKnownFile *pFile);
	// This is actually unused, but keep it here - will be needed later.
	void ClearKadSourcePublishInfo();

	/// Schedules AICH hashing for shared files with a missing or wrong AICH hash.
	void CheckAICHHashes(const std::list<CAICHHash> &hashes);

	/// Toggle automatic rescan of shared directories at runtime. Called when the user flips the
	/// pref in the Directories panel.
	void EnableDirectoryWatcher(bool enable);

	// Incremental-rescan entry points used by CSharedDirWatcher to apply one fs-watcher event
	// without re-walking every shared dir. fullPath is the raw filesystem path of the affected
	// entry. NotifyPathAdded queues hashing for an unknown file and no-ops if it is already
	// shared. NotifyPathRemoved detaches the CKnownFile m_pathIndex maps the path to.
	// NotifyPathModified treats a content change as remove-then-add when mtime/size have
	// shifted.
	//
	// All three are safe to call from the wxFileSystemWatcher event thread (wx's main thread on
	// every supported backend) and take list_mut internally.
	//
	// bulkScan: the caller is walking a whole tree (the watcher's new-subdirectory race scan)
	// rather than reacting to one event. Then an already-known file is counted, not announced
	// -- moving a large known tree into a recursive share would otherwise emit thousands of
	// info lines in one debounce flush.
	void NotifyPathAdded(const wxString &fullPath, bool bulkScan = false);
	void NotifyPathRemoved(const wxString &fullPath);
	void NotifyPathModified(const wxString &fullPath);

	// Detach every shared file under `dirPath`, for a renamed or deleted dir whose subtree
	// moves with no per-file events. The prefix match is separator-anchored, so ".../Season 1"
	// does not swallow ".../Season 10".
	void NotifyDirRemoved(const wxString &dirPath);

private:
	typedef std::list<CThreadTask *> TaskList;

	bool AddFile(CKnownFile *pFile);

	// Re-key m_pathIndex for an already-shared file whose on-disk path changed. A partfile
	// shared while downloading is keyed under Temp (or "" before SetFilePath ran) and moves to
	// Incoming on completion, but AddFile writes m_pathIndex only on a fresh insert, so the re-
	// add leaves the index stale. Drops any keys pointing at `file` and installs its current
	// one, so the watcher can resolve a later DELETE. Takes list_mut.
	void RefreshPathIndex(CKnownFile *file);

	// Invoked by AddFile with list_mut held. Kicks off a CMediaProbeTask when the preference is
	// enabled, the file looks like media by ED2K file type, and it has not been probed yet;
	// returns silently if any gate fails.
	//
	// The mode says WHICH gates to bypass: the two callers that bypass anything need different
	// ones, and a single "force" flag conflated them.
	//
	//  * Normal     -- both gates apply.
	//  * Completion -- both bypassed. The local probe must overwrite metadata inherited from the
	//                  search result, and a just-completed download is STILL a CPartFile. Safe
	//                  only because this fires exactly when the file has finished.
	//  * Refresh    -- metadata gate bypassed, partfile guard not: a genuinely incomplete
	//                  download has no complete file to read.
	enum class MediaProbeMode
	{
		Normal,
		Completion,
		Refresh,
	};
	// Returns true when a probe was actually enqueued, so a caller can report what it did. Do
	// NOT infer that from the worker's pending count: CMediaProbeThread::Entry swaps the whole
	// job list out as soon as it is signalled, so against an idle worker every enqueue reads as
	// a no-op.
	//
	// `bulk` says whether this probe belongs to a mass operation rather than to one file the
	// user is looking at. It decides logging verbosity only, and is passed rather than
	// inferred: the worker drains whatever is queued when it wakes.
	bool MaybeScheduleMediaProbe(
		CKnownFile *pFile, MediaProbeMode mode = MediaProbeMode::Normal, bool bulk = false);

	// Per-path attach: stat fname under directory, look it up in known.met, and either
	// AddFile() the existing CKnownFile or push a CHashingTask. Shared between the bulk-Reload
	// walk and the per-event watcher dispatch so the two agree on what counts as shareable.
	//
	// notifyGuiOnKnownAdd: the bulk-Reload path repaints the whole view afterwards and leaves
	// this false. The incremental watcher path has no such follow-up and passes true, or a re-
	// shared file (a rename in Incoming to a name already in known.met) would update the core
	// share set but never reach the GUI view.
	enum AddPathResult
	{
		//! Broken link, zero size, stat failed.
		kAddPathSkipped,
		//! Name matched the user's exclusion filter.
		kAddPathExcluded,
		//! Matched a CKnownFile and was newly attached.
		kAddPathKnown,
		// Matched a CKnownFile ALREADY in the share set, so the add was declined: the same
		// content reachable from a second shared directory. (Not a repeated watcher event
		// -- NotifyPathAdded returns on an index hit before reaching AddPathToShares.)
		// Split out from kAddPathKnown so callers announcing a file becoming shared do not
		// claim a share that did not happen.
		//
		// The file's path is left as it was, which is load-bearing: AddFile writes
		// m_pathIndex only on a fresh insert, so stamping the second directory onto the
		// file would leave GetFilePath() disagreeing with the key it is indexed under, and
		// nothing reconciles the two.
		kAddPathAlreadyShared,
		//! Unknown file; a CHashingTask was pushed.
		kAddPathQueued
	};
	AddPathResult AddPathToShares(const CPath &directory,
		const CPath &fname,
		TaskList &hashTasks,
		bool notifyGuiOnKnownAdd = false);
	// scanned/aborted are in/out: the caller passes a running count and a flag the
	// dir walker flips on abort, so one counter spans every path in a Reload().
	unsigned AddFilesFromDirectory(const CPath &directory,
		TaskList &hashTasks,
		const ReloadYieldCb &yieldCb,
		size_t &scanned,
		size_t &excluded,
		bool &aborted);
	void FindSharedFiles(const ReloadYieldCb &yieldCb, bool &aborted);
	// Atomic: RemoveFile() reads it off the upload worker thread (issue #1028).
	std::atomic<bool> reloading;
	// Set by RequestReload(), drained by Process(). Its rules -- coalescing, a mid-walk request
	// belonging to the next walk, an aborted walk giving its request back -- live in the latch
	// so they can be tested standalone.
	CSharedFilesReloadLatch m_reloadLatch;

	// New files discovered since the last Process() tick, counted at the one place discovery is
	// decided (AddPathToShares' queued branch) so every route is covered without plumbing.
	// Flushed once per tick, which coalesces a batch into a single line. A plain unsigned is
	// enough: every increment and the flush run on the main thread.
	unsigned m_discoveredNewFiles = 0;

	//! Already-known files attached during a bulk subdirectory scan, summarised
	//! by Process() rather than announced one line each. See NotifyPathAdded.
	unsigned m_attachedKnownFiles = 0;

	void SendListToServer();
	uint64 m_lastPublishED2K;
	bool m_lastPublishED2KFlag;

	CKnownFileList *filelist;

	CKnownFileMap m_Files_map;

	/**
	 * Shared files grouped by directory, for GetSharedFilesByDirectory().
	 *
	 * A peer browsing a share asks for one directory at a time, and answering each request used
	 * to walk the whole of m_Files_map calling CPath::IsSameDir(), which normalises both sides
	 * every time. That is O(directories x files): a 39,450-file share across 1,691 directories
	 * cost 66.7 million comparisons and blocked the main loop for the best part of a minute
	 * (issue #898).
	 *
	 * Keyed by CPath::GetDirKey(), the canonical form IsSameDir() reduces to, so grouping by it
	 * is equivalent to the walk it replaces. m_dirGroupsAt records the list generation it was
	 * built from; that generation is bumped under list_mut at every mutation of m_Files_map,
	 * which is the lock this is read and written under, so no further invalidation is needed.
	 */
	std::map<wxString, CKnownFilePtrList> m_dirGroups;
	//! Generation m_dirGroups was built from; see there.
	uint64 m_dirGroupsAt = 0;
	//! Whether m_dirGroups has been built at all; generation 0 is legal.
	bool m_dirGroupsBuilt = false;
	// See GetListGeneration(). Bumped under list_mut wherever m_Files_map
	// gains or loses an entry; atomic so it can be read without the lock.
	std::atomic<uint64> m_listGeneration{ 0 };
	// Secondary index keyed by full path so the watcher can resolve a DELETE/RENAME event to
	// its CKnownFile* in O(1) without walking m_Files_map. Maintained alongside it in AddFile()
	// and RemoveFile(), both under list_mut. Key is the file's current
	// GetFilePath().JoinPaths(GetFileName()) raw string.
	//
	// The invariant is "if and only if": an entry exists for a path exactly when a file in
	// m_Files_map lives there. NotifyPathAdded, NotifyPathModified and NotifyDirRemoved all
	// read a hit as proof the file is already shared, so an entry that outlives its file makes
	// the watcher silently refuse to share that path, returning before its first log statement.
	//
	// Hence FindSharedFiles clears it in the same locked scope as m_Files_map: the keys a
	// reload cannot heal are exactly the ones whose files the walk no longer finds (issue
	// #1028). Both containers are empty from that clear until the walk refills them, and
	// neither may be observed in between.
	std::unordered_map<wxString, CKnownFile *> m_pathIndex;
	mutable wxMutex list_mut;

	StringPathMap m_PublicSharedDirNames; //! used for mapping strings to shared directories

	/**
	 * The reverse of m_PublicSharedDirNames, keyed by CPath::GetDirKey().
	 *
	 * GetPublicSharedDirName() used to find a directory's public name by walking
	 * m_PublicSharedDirNames and comparing each entry with IsSameDir(), which normalises both
	 * paths. SendSharedDirectories() calls it once per shared directory, so that walk was
	 * O(directories^2): with 1,691 of them it was one half of a 53 s freeze while answering a
	 * browse (issue #898).
	 *
	 * Cleared wherever m_PublicSharedDirNames is, since the two are written together and expire
	 * together.
	 */
	std::map<wxString, wxString> m_publicNameByDirKey;

	/**
	 * Keys of every shared directory, for IsShared().
	 *
	 * The other half of the same freeze: IsShared() compared the path against every entry of
	 * shareddir_list and every category path with IsSameDir(), and GetPublicSharedDirName()
	 * calls it per directory as its safety check. Built on demand and dropped in Reload(),
	 * where the shared-directory set can change and the public names are already discarded.
	 */
	mutable std::set<wxString> m_sharedDirKeys;
	mutable bool m_sharedDirKeysBuilt = false;

	/* Kad Stuff */
	CPublishKeywordList *m_keywords;
	unsigned int m_currFileSrc;
	unsigned int m_currFileNotes;
	unsigned int m_currFileKey;
	uint32 m_lastPublishKadSrc;
	uint32 m_lastPublishKadNotes;

	// Fs-watcher for auto-rescan of shared dirs. Owned here, created lazily on
	// EnableDirectoryWatcher(true), forward-declared to keep wx/fswatcher.h out of public
	// includes.
	CSharedDirWatcher *m_dirWatcher;
};

#endif // SHAREDFILELIST_H
// File_checked_for_headers
