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

	// Yield/cancel hook for chunked reloads, invoked periodically during the
	// directory walk with the running count of files scanned. Returning false
	// aborts the reload promptly and leaves whatever was added in place. null is a
	// no-op, for daemon-side and EC-triggered callers with no UI to drive.
	using ReloadYieldCb = std::function<bool(size_t /*filesScanned*/)>;

	void Reload();
	// Cancellable + progress-reporting variant. Returns true if the
	// walk completed normally, false if `yieldCb` requested abort.
	bool Reload(ReloadYieldCb yieldCb);

	// Ask for a full shared-files reload to run from the next Process() tick
	// instead of inline in the caller. Callers that sit on the core event loop --
	// every EC request handler, and the directory watcher's dropped-events
	// fallback -- use this so they can answer immediately rather than blocking for
	// the whole walk. On a large or network-mounted share tree that walk takes
	// seconds to minutes, and since amuleapi's EC lane is a single serialised
	// worker, a blocking reload there stalls the refresher and turns unrelated
	// endpoints into 503s.
	//
	// Repeat requests before the tick coalesce into one walk, and a request
	// arriving mid-walk keeps the flag set so it runs afterwards instead of
	// nesting.
	//
	// A plain bool is deliberate: every setter and the reader run on the core
	// event loop. A caller off that thread is the thing to fix -- do not make this
	// atomic.
	void RequestReload() { m_reloadLatch.Request(); }

	// True when a RequestReload() is outstanding. GUI callers use this to run the
	// owed walk themselves behind a progress dialog rather than letting it land
	// silently on a Process() tick.
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

	// Re-extract media metadata for every shared file, whether or not it has any
	// already. Returns how many probes were queued.
	//
	// This is the only way to correct a file whose metadata is wrong rather than
	// missing: the scheduler skips anything that already carries a media tag, so a
	// value stored by an older build is otherwise permanent short of deleting
	// known.met, which would also discard the ed2k part hashes and every per-file
	// statistic.
	//
	// Asynchronous: the work is queued on the media-probe worker. Nothing else
	// about a file is touched -- it is not re-hashed and never leaves the share.
	unsigned RefreshAllMediaMetadata();

	// The single-file form, addressed by hash. Returns false when no shared
	// file has that hash, or it is not eligible: not audio/video by
	// extension, or an incomplete download.
	bool RefreshMediaMetadata(const CMD4Hash &hash);

	// The batched form the GUI uses; returns how many probes were queued. Exists
	// so amulegui can send ONE EC request for a selection: its request fifo stalls
	// the GUI's own polling past about twenty in flight, and a "select all,
	// refresh" would otherwise put one packet per shared file into the socket.
	unsigned RefreshMediaMetadata(const std::vector<CMD4Hash> &hashes);

	/**
	 * Returns the name of a folder visible to the public.
	 *
	 * @param dir The full path to a shared directory.
	 * @return The name of the shared directory that will be visible to the public.
	 *
	 * This function is used to hide sensitive data considering the directory structure of the client.
	 * The returned public name consists of only subdirectories that are shared.
	 * Example: /ed2k/shared/games/tetris -> "games/tetris" if /ed2k/shared are not marked as shared
	 */
	wxString GetPublicSharedDirName(const CPath &dir);
	const CPath *GetDirForPublicSharedDirName(const wxString &strSharedDir) const;

	/**
	 * Returns true, if the specified path points to a shared directory or single shared file.
	 */
	bool IsShared(const CPath &path) const;

	/* Kad Stuff */
	void Publish();
	void AddKeywords(CKnownFile *pFile);
	void RemoveKeywords(CKnownFile *pFile);
	// This is actually unused, but keep it here - will be needed later.
	void ClearKadSourcePublishInfo();

	/**
	 * Checks for files which missing or wrong AICH hashes.
	 * Those that are found are scheduled for ACIH hashing.
	 */
	void CheckAICHHashes(const std::list<CAICHHash> &hashes);

	/**
	 * Toggle automatic rescan of shared directories at runtime.
	 * Called when the user flips the corresponding pref in the
	 * Directories panel.
	 */
	void EnableDirectoryWatcher(bool enable);

	// Incremental-rescan entry points used by CSharedDirWatcher to apply a single
	// fs-watcher event without re-walking every shared dir. fullPath is the raw
	// filesystem path of the affected entry.
	//
	// NotifyPathAdded queues a hashing task for an unknown file and no-ops if the
	// file is already shared. NotifyPathRemoved looks the path up in m_pathIndex
	// and detaches the matching CKnownFile. NotifyPathModified treats a
	// content-change as remove-then-add when mtime/size have shifted.
	//
	// All three are safe to call from the wxFileSystemWatcher event thread (wx's
	// main thread on every supported backend) and take list_mut internally.
	//
	// bulkScan: the caller is walking a whole directory tree (the watcher's
	// new-subdirectory race scan) rather than reacting to a single event. In that
	// mode an already-known file is counted rather than announced individually --
	// moving a large known tree into a recursive share would otherwise emit
	// thousands of info lines in one debounce flush on the core event loop.
	void NotifyPathAdded(const wxString &fullPath, bool bulkScan = false);
	void NotifyPathRemoved(const wxString &fullPath);
	void NotifyPathModified(const wxString &fullPath);

	// Detach every shared file under `dirPath` (separator-anchored prefix, so
	// ".../Season 1" does not swallow sibling ".../Season 10"). For a renamed
	// or deleted dir, whose subtree moves with no per-file events.
	void NotifyDirRemoved(const wxString &dirPath);

private:
	typedef std::list<CThreadTask *> TaskList;

	bool AddFile(CKnownFile *pFile);

	// Re-key m_pathIndex for an already-shared file whose on-disk path changed
	// since it was added. A partfile shared while downloading is keyed under the
	// Temp dir (or "" before SetFilePath ran) and moves to Incoming on completion,
	// but AddFile only writes m_pathIndex on a fresh insert, so the re-add leaves
	// the index pointing at the stale path. Drops any keys pointing at `file` and
	// installs its current one, so the dir-watcher can resolve a later DELETE.
	// Takes list_mut.
	void RefreshPathIndex(CKnownFile *file);

	// Invoked by AddFile once list_mut is held. Kicks off a CMediaProbeTask when
	// the preference is enabled, the file looks like media by ED2K file type, and
	// it has not been probed yet; returns silently if any gate fails.
	//
	// The mode says WHICH gates to bypass, because the two callers that bypass
	// anything need different ones and a single "force" flag conflated them:
	//
	//  * Normal     -- both gates apply.
	//  * Completion -- both bypassed. The authoritative local probe must overwrite
	//                  metadata inherited from the search result, and a
	//                  just-completed download is STILL a CPartFile, so the
	//                  partfile guard has to lift too. Safe only because this
	//                  fires exactly when the file has finished.
	//  * Refresh    -- the metadata gate is bypassed, the partfile guard is not: a
	//                  genuinely incomplete download is in the shared list and has
	//                  no complete file to read.
	enum class MediaProbeMode
	{
		Normal,
		Completion,
		Refresh,
	};
	// Returns true when a probe was actually enqueued, so a caller can report what
	// it did. Do NOT infer that from the worker's pending count:
	// CMediaProbeThread::Entry swaps the whole job list out as soon as it is
	// signalled, so against an idle worker every enqueue reads as a no-op.
	//
	// `bulk` says whether this probe belongs to a mass operation rather than to
	// one file the user is looking at. It decides logging verbosity only, and is
	// passed rather than inferred downstream: the worker cannot tell, because it
	// drains whatever happens to be queued when it wakes.
	bool MaybeScheduleMediaProbe(
		CKnownFile *pFile, MediaProbeMode mode = MediaProbeMode::Normal, bool bulk = false);

	// Per-path attach: stat fname under directory, look it up in known.met, and
	// either AddFile() the existing CKnownFile or push a CHashingTask onto
	// hashTasks. Shared between the bulk-Reload walk and the per-event watcher
	// dispatch so the two agree on what counts as shareable.
	//
	// notifyGuiOnKnownAdd: the bulk-Reload path repaints the whole view once the
	// walk finishes, so it leaves this false. The incremental watcher path has no
	// such follow-up and passes true, or a re-shared file (a rename in Incoming to
	// a name already in known.met) would update the core share set but never reach
	// the GUI view.
	enum AddPathResult
	{
		//! Broken link, zero size, stat failed.
		kAddPathSkipped,
		//! Name matched the user's exclusion filter.
		kAddPathExcluded,
		//! Matched a CKnownFile and was newly attached.
		kAddPathKnown,
		// Matched a CKnownFile ALREADY in the share set, so the add was declined:
		// the same content reachable from a second shared directory. (Not a
		// repeated watcher event -- NotifyPathAdded returns on an index hit before
		// reaching AddPathToShares.) Split out from kAddPathKnown so callers
		// announcing a file becoming shared do not claim a share that did not
		// happen.
		//
		// The file's path is left exactly as it was, which is load-bearing:
		// AddFile writes m_pathIndex only on a fresh insert, so stamping the
		// second directory onto the file would leave GetFilePath() disagreeing
		// with the key it is indexed under, and nothing reconciles the two.
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
	// Set by RequestReload(), drained by Process(). The rules it enforces --
	// coalescing, a mid-walk request belonging to the next walk, an aborted walk
	// giving its request back -- live in the latch so they can be tested without a
	// CSharedFileList.
	CSharedFilesReloadLatch m_reloadLatch;

	// New files discovered since the last Process() tick, counted at the one place
	// discovery is decided (AddPathToShares' queued branch) so every route is
	// covered without plumbing. Flushed once per tick, which coalesces a batch of
	// files into a single line.
	//
	// A plain unsigned is enough: every increment and the flush run on the main
	// thread.
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
	 * A peer browsing a share asks for one directory at a time, and answering
	 * each request used to walk the whole of m_Files_map calling
	 * CPath::IsSameDir(), which normalises both sides every time. That is
	 * O(directories x files): a 39,450-file share across 1,691 directories
	 * cost 66.7 million comparisons, blocking the main loop for the best part
	 * of a minute (issue #898).
	 *
	 * Keyed by CPath::GetDirKey(), which is the same canonical form
	 * IsSameDir() reduces to, so grouping by it is equivalent to the walk it
	 * replaces. m_dirGroupsAt records the list generation it was built from;
	 * the generation is already bumped under list_mut at every mutation of
	 * m_Files_map, which is the lock this is read and written under, so no
	 * further invalidation is needed.
	 */
	std::map<wxString, CKnownFilePtrList> m_dirGroups;
	//! Generation m_dirGroups was built from; see there.
	uint64 m_dirGroupsAt = 0;
	//! Whether m_dirGroups has been built at all; generation 0 is legal.
	bool m_dirGroupsBuilt = false;
	// See GetListGeneration(). Bumped under list_mut wherever m_Files_map
	// gains or loses an entry; atomic so it can be read without the lock.
	std::atomic<uint64> m_listGeneration{ 0 };
	// Secondary index keyed by full path so the watcher can resolve a
	// DELETE/RENAME event to its CKnownFile* in O(1) without walking m_Files_map.
	// Maintained alongside it in AddFile() and RemoveFile(), both under list_mut.
	// Key is the file's current GetFilePath().JoinPaths(GetFileName()) raw string.
	//
	// The invariant is "if and only if": an entry exists for a path exactly when a
	// file currently in m_Files_map lives there. NotifyPathAdded,
	// NotifyPathModified and NotifyDirRemoved all read a hit as proof the file is
	// already shared, so an entry that outlives its file makes the watcher
	// silently refuse to share that path -- it returns before its first log
	// statement, so nothing is reported at any level.
	//
	// This is why FindSharedFiles clears it in the same locked scope as
	// m_Files_map: the keys a reload cannot heal are exactly the ones whose files
	// the walk no longer finds (issue #1028). Both containers are empty from that
	// clear until the walk refills them, and neither may be observed in between.
	std::unordered_map<wxString, CKnownFile *> m_pathIndex;
	mutable wxMutex list_mut;

	StringPathMap m_PublicSharedDirNames; //! used for mapping strings to shared directories

	/**
	 * The reverse of m_PublicSharedDirNames, keyed by CPath::GetDirKey().
	 *
	 * GetPublicSharedDirName() used to find a directory's public name by
	 * walking m_PublicSharedDirNames and comparing each entry with
	 * IsSameDir(), which normalises both paths. SendSharedDirectories()
	 * calls it once per shared directory, so that walk was O(directories^2):
	 * with 1,691 shared directories it was one of the two halves of a 53 s
	 * freeze while answering a browse (issue #898).
	 *
	 * Cleared wherever m_PublicSharedDirNames is, since the two are written
	 * together and expire together.
	 */
	std::map<wxString, wxString> m_publicNameByDirKey;

	/**
	 * Keys of every shared directory, for IsShared().
	 *
	 * The other half of the same freeze: IsShared() compared the path against
	 * every entry of shareddir_list and every category path with IsSameDir(),
	 * and GetPublicSharedDirName() calls it per directory as its safety
	 * check. Built on demand and dropped in Reload(), which is where the
	 * shared-directory set can change and where the public names are already
	 * discarded for the same reason.
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
	// EnableDirectoryWatcher(true), forward-declared to keep wx/fswatcher.h out of
	// public includes.
	CSharedDirWatcher *m_dirWatcher;
};

#endif // SHAREDFILELIST_H
// File_checked_for_headers
