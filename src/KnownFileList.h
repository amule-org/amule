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

#ifndef KNOWNFILELIST_H
#define KNOWNFILELIST_H

#include <unordered_set>

#include "SharedFileList.h" // CKnownFileMap

class CKnownFile;
class CPath;
class CAICHHash;

class CKnownFileList
{
public:
	CKnownFileList();
	~CKnownFileList();
	bool SafeAddKFile(CKnownFile *toadd, bool afterHashing = false);
	bool Init();
	void Save();
	void Clear();
	CKnownFile *FindKnownFile(const CPath &filename, time_t in_date, uint64 in_size);
	CKnownFile *FindKnownFileByID(const CMD4Hash &hash);

	// Make `file` the record that hash-keyed lookups resolve to, and return whether that
	// changed anything.
	//
	// m_knownFileMap holds exactly one record per hash: whichever known.met entry was loaded
	// last, since Append demotes the earlier ones to m_duplicateFileList. For content that
	// exists at several paths that record is not necessarily the copy the share scan found on
	// disk, and once its own copy is deleted it is not on disk at all. FindKnownFileByID then
	// hands out a record whose path cannot be opened, which is what broke "verify local data",
	// the EC rename handler and the search "already known" flag for a deleted duplicate (issue
	// #1265). The cap/TTL prune cannot heal it: it only evicts duplicate-list records, and
	// keeps the dead live entry precisely because a same-hash record is shared.
	//
	// Called by the share scan for each file it actually shares, so the record backed by a file
	// we just saw wins the hash.
	bool PromoteToCanonical(CKnownFile *file);

	// Returns true iff `file` is still one of this list's records, canonical or duplicate. It
	// answers "does this record still exist", not "is it canonical": PromoteToCanonical demotes
	// live records, so a map-only answer would report a live file as destroyed. Pointer-value
	// comparison only -- `file` may already be freed, in which case this returns false without
	// dereferencing it. Used by the async-task completion handlers to validate an event's
	// `owner` pointer.
	bool IsKnownFile(const CKnownFile *file) const;

	/**
	 * Number of entries loaded from known.met. Used at startup as an estimate of how many files
	 * the shared-file scan is about to walk, since the real total is only known once the walk
	 * finishes and counting first would mean traversing the tree twice. It is last session's
	 * view of the same tree, so it is close for a stable share and merely approximate otherwise
	 * -- callers must treat it as a hint, not a total.
	 */
	size_t GetKnownFileCount() const;

	void PrepareIndex();
	void ReleaseIndex();

	// Latch set by CSharedFileList::Reload once a full share-scan has finished in this session.
	// PruneDuplicates only runs once this is true, so the cap-prune never fires before the pin
	// set is populated by FindKnownFile-during-scan.
	void MarkInitialShareScanComplete();

	// Snapshot the AICH master hashes still referenced by live and duplicate-list records. Used
	// by CAICHSyncTask to prune orphaned hashsets out of known2_64.met -- entries whose owning
	// known.met record was TTL-evicted and no longer appears in either map. Populated under
	// list_mut for a consistent view.
	void CollectLiveAICHRoots(std::unordered_set<CAICHHash> &out);

	uint16 requested;
	uint32 transferred;
	uint16 accepted;

private:
	mutable wxMutex list_mut;

	bool Append(CKnownFile *, bool afterHashing = false);

	CKnownFile *IsOnDuplicates(const CPath &filename, uint32 in_date, uint64 in_size) const;

	bool KnownFileMatches(
		CKnownFile *knownFile, const CPath &filename, uint32 in_date, uint64 in_size) const;

	// Drop duplicate-list records whose hash has more than KNOWN_DUPLICATE_HASH_CAP variants,
	// keeping the newest by mtime. `inUse` is a snapshot of pointers currently held by
	// CSharedFileList::m_Files_map, which are never pruned. m_pinnedDuplicates additionally
	// protects records FindKnownFile matched against a real on-disk file this session, even
	// where AddFile rejected them as content-duplicates of an already-shared file.
	void PruneDuplicates(const std::unordered_set<CKnownFile *> &inUse);

	typedef std::list<CKnownFile *> KnownFileList;
	KnownFileList m_duplicateFileList;
	CKnownFileMap m_knownFileMap;
	// The filename "known.met"
	wxString m_filename;
	// Speeds up the shared-files reload. The key is (size, mtime) rather than size alone: a
	// library with many files of the same size would otherwise collapse FindKnownFile()'s
	// equal_range into a large bucket that KnownFileMatches walks linearly, turning the reload
	// into O(N^2) over the same-size files. Adding mtime narrows the bucket aggressively in any
	// realistic library, at 8 bytes of key against the previous 4.
	typedef std::multimap<std::pair<uint32, uint32>, CKnownFile *> KnownFileSizeMap;
	KnownFileSizeMap *m_knownSizeMap;
	KnownFileSizeMap *m_duplicateSizeMap;

	// Drop `record`'s entry from one of the (size, mtime) indexes above. Each index mirrors a
	// list, so a record moving between the live map and the duplicate list has to be moved here
	// too, or FindKnownFile hands out a pointer that no longer belongs to the list it was
	// indexed under. Takes the index rather than reading the member, because every caller moves
	// a record in one specific direction.
	void EraseFromSizeMap(KnownFileSizeMap *sizeMap, CKnownFile *record);

	// Add `record` to one of the indexes above, keyed the same way EraseFromSizeMap looks it
	// up. The two must agree on the key, so they live side by side.
	void InsertIntoSizeMap(KnownFileSizeMap *sizeMap, CKnownFile *record);

	// Duplicate-list records that FindKnownFile / IsOnDuplicates returned during this session,
	// i.e. whose (name, date, size) matched a real on-disk file. Pinned for the rest of the
	// session so the cap-prune never drops a record known to represent a live file, which would
	// mean re-hashing on the next restart. Cleared on Clear() / Init().
	std::unordered_set<CKnownFile *> m_pinnedDuplicates;

	// Set to true by MarkInitialShareScanComplete() at the end of the
	// first non-aborted CSharedFileList::Reload of the session.
	bool m_initialShareScanComplete;
};

#endif // KNOWNFILELIST_H
// File_checked_for_headers
