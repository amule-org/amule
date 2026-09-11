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

#include "KnownFileList.h" // Interface declarations
#include "GuiEvents.h"     // Notify_KnownFileBeingDestroyed

#include <common/DataFileVersion.h>

#include <algorithm>
#include <map>
#include <memory> // Do_not_auto_remove (lionel's Mac, 10.3)
#include <set>
#include <vector>
#include "DownloadQueue.h" // Needed for theApp->downloadqueue access
#include "PartFile.h"      // Needed for CPartFile
#include "amule.h"
#include "Logger.h"
#include "MemFile.h"
#include "ScopedPtr.h"
#include "SearchList.h" // Needed for UpdateSearchFileByHash
#include "SharedFileList.h"
#include <common/Format.h>
#include "Preferences.h" // Needed for thePrefs

// Max duplicate-list records retained per hash. Unique hashes always keep their live m_knownFileMap
// entry; this caps only the historical (name/date) variants in m_duplicateFileList. 8 covers daily-
// touch / weekly-snapshot / monthly-backup cycles while bounding known.met at unique_hashes * (1 +
// cap).
#define KNOWN_DUPLICATE_HASH_CAP 8

// TTL after which a record (live or duplicate) whose lastSeen has not been refreshed is dropped. A
// file on disk has its lastSeen bumped by FindKnownFile / IsOnDuplicates / Append every share-scan;
// anything stale this long either lost its file or had its mtime/name change in a way that will not
// recur (mtime is monotone-forward in practice). 30 days catches most pathological touch loops
// without losing intermittent matches.
#define KNOWN_DUPLICATE_TTL_SECS (30 * 24 * 60 * 60)

// This function is inlined for performance
inline bool CKnownFileList::KnownFileMatches(
	CKnownFile *knownFile, const CPath &filename, uint32 in_date, uint64 in_size) const
{
	return (knownFile->GetLastChangeDatetime() == (time_t)in_date) &&
	       (knownFile->GetFileSize() == in_size) && (knownFile->GetFileName() == filename);
}

CKnownFileList::CKnownFileList()
{
	accepted = 0;
	requested = 0;
	transferred = 0;
	m_filename = "known.met";
	m_knownSizeMap = NULL;
	m_duplicateSizeMap = NULL;
	m_initialShareScanComplete = false;
	Init();
}

CKnownFileList::~CKnownFileList()
{
	Clear();
}

bool CKnownFileList::Init()
{
	CFile file;

	CPath fullpath = CPath(thePrefs::GetConfigDir() + m_filename);
	if (!fullpath.FileExists()) {
		// This is perfectly normal. The file was probably either
		// deleted, or this is the first time running aMule.
		return false;
	}

	if (!file.Open(fullpath)) {
		AddLogLineC(CFormat(_("WARNING: %s cannot be opened.")) % m_filename);
		return false;
	}

	try {
		uint8 version = file.ReadUInt8();
		if ((version != MET_HEADER) && (version != MET_HEADER_WITH_LARGEFILES)) {
			AddLogLineC(_("WARNING: Known file list corrupted, contains invalid header."));
			return false;
		}

		wxMutexLocker sLock(list_mut);
		uint32 RecordsNumber = file.ReadUInt32();
		AddDebugLogLineN(logKnownFiles,
			CFormat("Reading %i known files from file format 0x%2.2x.") % RecordsNumber %
				version);

		// Keep the size-map index live during the load. Append() is O(log N) per record,
		// but on each MD4 hash collision it falls back to IsOnDuplicates(name, date, size),
		// which without a duplicate-size index scans m_duplicateFileList linearly -- O(N^2)
		// over the whole load. Prebuilding the empty index lets Append maintain it
		// incrementally, so every collision check takes the equal_range fast path (issue
		// #562, a ~36 s startup gap on a 200 k-file library).
		PrepareIndex();
		for (uint32 i = 0; i < RecordsNumber; i++) {
			CScopedPtr<CKnownFile> record;
			if (record->LoadFromFile(&file)) {
				AddDebugLogLineN(logKnownFiles,
					CFormat("Known file read: %s") % record->GetFileName());
				Append(record.release());
			} else {
				AddLogLineC(
					_("Failed to load entry in known file list, file may be corrupt"));
			}
		}
		ReleaseIndex();
		AddDebugLogLineN(logKnownFiles, "Finished reading known files");

		return true;
	} catch (const CInvalidPacket &e) {
		ReleaseIndex();
		AddLogLineC(_("Invalid entry in known file list, file may be corrupt: ") + e.what());
	} catch (const CSafeIOException &e) {
		ReleaseIndex();
		AddLogLineC(CFormat(_("IO error while reading %s file: %s")) % m_filename % e.what());
	}

	return false;
}

void CKnownFileList::Save()
{
	// Acquire the lock before opening the .new file. Save() is called from both the main thread
	// and the hashing worker (CHashingTask::OnLastTask); if two callers raced past the open,
	// both would create known.met.new at the same path, the first to Close() would rename it
	// away, and the second's rename would fail with ENOENT (#86). Holding list_mut around the
	// whole save serialises the .new lifecycle; the list is read-only inside, so the critical
	// section is not meaningfully widened.
	wxMutexLocker sLock(list_mut);

	// Snapshot the in-use set under our own lock. Taking it before locking left a TOCTOU window
	// where the main thread could add a CKnownFile to sharedfiles between snapshot and prune;
	// the prune then deleted a file sharedfiles still indexed, and the EC encoder map kept
	// feeding the dangling pointer to Get_EC_Response_GetUpdate (#685).
	//
	// Brief overlap of the knownfiles -> sharedfiles / downloadqueue locks is safe: nothing
	// acquires those in the reverse order while holding the first.
	std::unordered_set<CKnownFile *> inUse;
	if (theApp && theApp->sharedfiles) {
		std::vector<CKnownFile *> sharedSnapshot;
		theApp->sharedfiles->CopyFileList(sharedSnapshot);
		inUse.insert(sharedSnapshot.begin(), sharedSnapshot.end());
	}
	if (theApp && theApp->downloadqueue) {
		std::vector<CPartFile *> dqSnapshot;
		theApp->downloadqueue->CopyFileList(dqSnapshot, true);
		inUse.insert(dqSnapshot.begin(), dqSnapshot.end());
	}

	PruneDuplicates(inUse);

	CFile file(thePrefs::GetConfigDir() + m_filename, CFile::write_safe);
	if (!file.IsOpened()) {
		return;
	}

	AddDebugLogLineN(logKnownFiles, CFormat("start saving %s") % m_filename);

	try {
		// Kry - This is the version, but we do not know it until we know whether any large
		// file is saved. This keeps the list compatible with previous versions.
		bool bContainsAnyLargeFiles = false;
		file.WriteUInt8(0);

		file.WriteUInt32(m_knownFileMap.size() + m_duplicateFileList.size());

		// Duplicates handling. Duplicates needs to be saved first,
		// since it is the last entry that gets used.
		KnownFileList::iterator itDup = m_duplicateFileList.begin();
		for (; itDup != m_duplicateFileList.end(); ++itDup) {
			(*itDup)->WriteToFile(&file);
			if ((*itDup)->IsLargeFile()) {
				bContainsAnyLargeFiles = true;
			}
		}

		CKnownFileMap::iterator it = m_knownFileMap.begin();
		for (; it != m_knownFileMap.end(); ++it) {
			it->second->WriteToFile(&file);
			if (it->second->IsLargeFile()) {
				bContainsAnyLargeFiles = true;
			}
		}

		file.Seek(0);
		file.WriteUInt8(bContainsAnyLargeFiles ? MET_HEADER_WITH_LARGEFILES : MET_HEADER);
		file.Close();
	} catch (const CIOFailureException &e) {
		AddLogLineC(CFormat(_("Error while saving %s file: %s")) % m_filename % e.what());
	}
	AddDebugLogLineN(logKnownFiles, CFormat("finished saving %s") % m_filename);
}

size_t CKnownFileList::GetKnownFileCount() const
{
	wxMutexLocker sLock(list_mut);
	return m_knownFileMap.size();
}

bool CKnownFileList::IsKnownFile(const CKnownFile *file) const
{
	// Pointer-value scan over both lists; safe to call with a possibly-freed `file` pointer,
	// since nothing is dereferenced. Used by OnFinishedHashing / OnFinishedAICHHashing to
	// validate that the owner pointer survived hashing. Neither container is pointer-keyed, so
	// both are walked: linear in shareset size, but only on hash completion, which is rare
	// enough not to justify a per-pointer index.
	wxMutexLocker sLock(list_mut);
	for (const auto &entry : m_knownFileMap) {
		if (entry.second == file) {
			return true;
		}
	}
	// The duplicate list counts as alive: this asks whether the record still exists, not
	// whether it is canonical. PromoteToCanonical moves a live record out of the map whenever
	// the share scan finds a better copy for its hash, and an AICH result landing after that
	// must not read as "owner was destroyed" -- the result would be dropped with a log line
	// saying the opposite of what happened. A record that really was freed is erased from this
	// list by PruneDuplicates under the same lock, so the freed case still answers false.
	for (const CKnownFile *record : m_duplicateFileList) {
		if (record == file) {
			return true;
		}
	}
	return false;
}

void CKnownFileList::Clear()
{
	wxMutexLocker sLock(list_mut);

	// Fire Notify_KnownFileBeingDestroyed for every file about to be deleted, so subscribers
	// (list ctrls, dialogs, the AICH static list, the write thread's flushList, EC client-side
	// m_uploadingfile / m_reqfile) strip their references before the delete. Pointer-value
	// comparison only: the objects are alive at notify time, but subscribers must not deref
	// them on the main-thread dispatch, which may run after DeleteContents has freed them.
	for (CKnownFileMap::const_iterator it = m_knownFileMap.begin(); it != m_knownFileMap.end(); ++it) {
		Notify_KnownFileBeingDestroyed(it->second);
	}
	for (KnownFileList::const_iterator it = m_duplicateFileList.begin(); it != m_duplicateFileList.end();
		++it) {
		Notify_KnownFileBeingDestroyed(*it);
	}

	DeleteContents(m_knownFileMap);
	DeleteContents(m_duplicateFileList);
	ReleaseIndex();
	m_pinnedDuplicates.clear();
	m_initialShareScanComplete = false;
}

void CKnownFileList::MarkInitialShareScanComplete()
{
	wxMutexLocker sLock(list_mut);
	m_initialShareScanComplete = true;
}

void CKnownFileList::CollectLiveAICHRoots(std::unordered_set<CAICHHash> &out)
{
	wxMutexLocker sLock(list_mut);
	out.reserve(out.size() + m_knownFileMap.size() + m_duplicateFileList.size());
	for (CKnownFileMap::const_iterator it = m_knownFileMap.begin(); it != m_knownFileMap.end(); ++it) {
		const CKnownFile *f = it->second;
		if (f && f->HasProperAICHHashSet()) {
			out.insert(f->GetAICHHashset()->GetMasterHash());
		}
	}
	// Duplicate-list records can also be the only owner of an AICH master hash, because a hash-
	// collision demote in Append parks the previous record and its hashset on the duplicate
	// list. Dropping the duplicate's AICH from known2_64.met would silently lose that hashset
	// if the duplicate were later re-promoted by an mtime restore; keeping both sets is cheap.
	for (KnownFileList::const_iterator it = m_duplicateFileList.begin(); it != m_duplicateFileList.end();
		++it) {
		const CKnownFile *f = *it;
		if (f && f->HasProperAICHHashSet()) {
			out.insert(f->GetAICHHashset()->GetMasterHash());
		}
	}
}

CKnownFile *CKnownFileList::FindKnownFile(const CPath &filename, time_t in_date, uint64 in_size)
{
	wxMutexLocker sLock(list_mut);
	const uint32 now = (uint32)time(NULL);

	if (m_knownSizeMap) {
		const auto key = std::make_pair((uint32)in_size, (uint32)in_date);
		std::pair<KnownFileSizeMap::const_iterator, KnownFileSizeMap::const_iterator> p;
		p = m_knownSizeMap->equal_range(key);
		for (KnownFileSizeMap::const_iterator it = p.first; it != p.second; ++it) {
			CKnownFile *cur_file = it->second;
			if (KnownFileMatches(cur_file, filename, in_date, in_size)) {
				cur_file->SetLastSeen(now);
				return cur_file;
			}
		}
	} else {
		for (CKnownFileMap::const_iterator it = m_knownFileMap.begin(); it != m_knownFileMap.end();
			++it) {
			CKnownFile *cur_file = it->second;
			if (KnownFileMatches(cur_file, filename, in_date, in_size)) {
				cur_file->SetLastSeen(now);
				return cur_file;
			}
		}
	}

	// Pin any duplicate-list match against this session's prune, so a real on-disk file's
	// record is not dropped just because its hash is also held by a more-recent live entry --
	// the dual-content-copy case, where the same hash sits in two shared paths and only one
	// becomes m_Files_map.
	CKnownFile *dup = IsOnDuplicates(filename, in_date, in_size);
	if (dup) {
		dup->SetLastSeen(now);
		m_pinnedDuplicates.insert(dup);
	}
	return dup;
}

CKnownFile *CKnownFileList::IsOnDuplicates(const CPath &filename, uint32 in_date, uint64 in_size) const
{
	if (m_duplicateSizeMap) {
		const auto key = std::make_pair((uint32)in_size, (uint32)in_date);
		std::pair<KnownFileSizeMap::const_iterator, KnownFileSizeMap::const_iterator> p;
		p = m_duplicateSizeMap->equal_range(key);
		for (KnownFileSizeMap::const_iterator it = p.first; it != p.second; ++it) {
			CKnownFile *cur_file = it->second;
			if (KnownFileMatches(cur_file, filename, in_date, in_size)) {
				return cur_file;
			}
		}
	} else {
		for (KnownFileList::const_iterator it = m_duplicateFileList.begin();
			it != m_duplicateFileList.end();
			++it) {
			CKnownFile *cur_file = *it;
			if (KnownFileMatches(cur_file, filename, in_date, in_size)) {
				return cur_file;
			}
		}
	}
	return NULL;
}

CKnownFile *CKnownFileList::FindKnownFileByID(const CMD4Hash &hash)
{
	wxMutexLocker sLock(list_mut);

	if (!hash.IsEmpty()) {
		if (m_knownFileMap.find(hash) != m_knownFileMap.end()) {
			return m_knownFileMap[hash];
		} else {
			return NULL;
		}
	}
	return NULL;
}

void CKnownFileList::EraseFromSizeMap(KnownFileSizeMap *sizeMap, CKnownFile *record)
{
	// Caller must hold list_mut.
	//
	// The key is recomputed from the record's CURRENT size and mtime, so a record whose either
	// value changed while it was indexed cannot be found and the erase silently leaves the old
	// entry behind. Every caller today either indexes a record it has not mutated or sets the
	// new values before the first insert, but a future caller that mutates an already-indexed
	// record has to erase under the old key first.
	if (!sizeMap) {
		return;
	}
	const auto key =
		std::make_pair((uint32)record->GetFileSize(), (uint32)record->GetLastChangeDatetime());
	const auto range = sizeMap->equal_range(key);
	for (auto hit = range.first; hit != range.second; ++hit) {
		if (hit->second == record) {
			sizeMap->erase(hit);
			return;
		}
	}
}

void CKnownFileList::InsertIntoSizeMap(KnownFileSizeMap *sizeMap, CKnownFile *record)
{
	// Caller must hold list_mut.
	if (!sizeMap) {
		return;
	}
	sizeMap->insert(std::make_pair(
		std::make_pair((uint32)record->GetFileSize(), (uint32)record->GetLastChangeDatetime()),
		record));
}

bool CKnownFileList::PromoteToCanonical(CKnownFile *file)
{
	if (!file) {
		return false;
	}

	wxMutexLocker sLock(list_mut);

	const CMD4Hash &tkey = file->GetFileHash();
	const auto it = m_knownFileMap.find(tkey);
	if (it == m_knownFileMap.end() || it->second == file) {
		// Already canonical, or this hash has no live record at all -- a freshly hashed
		// file becomes canonical through Append, so there is nothing to take over here.
		return false;
	}

	CKnownFile *demoted = it->second;

	// Same swap Append performs when a later known.met entry takes over a hash, minus the Kad
	// withdrawal: `demoted` cannot be in the shared list (m_Files_map is hash-keyed and `file`
	// holds that slot), and only CSharedFileList::AddFile publishes keywords, so it has none to
	// remove.
	m_duplicateFileList.push_back(demoted);
	EraseFromSizeMap(m_knownSizeMap, demoted);
	InsertIntoSizeMap(m_duplicateSizeMap, demoted);

	// `file` moves the other way. It is on the duplicate list in the case this exists for, but
	// not necessarily -- an already-canonical record short-circuits above.
	m_duplicateFileList.remove(file);
	EraseFromSizeMap(m_duplicateSizeMap, file);
	InsertIntoSizeMap(m_knownSizeMap, file);
	m_knownFileMap[tkey] = file;

	// m_pinnedDuplicates keeps `file` on purpose: the pin records that this session matched the
	// record against a real on-disk file, which stays true. It only ever guards duplicate-list
	// records, so it costs nothing while `file` is canonical and protects it from the cap prune
	// if a later Append demotes it.

	AddDebugLogLineN(logKnownFiles,
		CFormat("Duplicate '%s' is now the canonical record for its hash, replacing '%s'") %
			file->GetFileName().GetPrintable() % demoted->GetFileName().GetPrintable());
	return true;
}

bool CKnownFileList::SafeAddKFile(CKnownFile *toadd, bool afterHashing)
{
	bool ret;
	{
		wxMutexLocker sLock(list_mut);
		ret = Append(toadd, afterHashing);
	}
	if (ret) {
		theApp->searchlist->UpdateSearchFileByHash(toadd->GetFileHash());
	}
	return ret;
}

bool CKnownFileList::Append(CKnownFile *Record, bool afterHashing)
{
	if (Record->GetFileSize() > 0) {
		// sanity check if the number of part hashes is correct here
		if (Record->GetHashCount() != Record->GetED2KPartHashCount()) {
			AddDebugLogLineC(logKnownFiles,
				CFormat("%s with size %d should have %d part hashes, but only %d are "
					"available") %
					Record->GetFileName().GetPrintable() % Record->GetFileSize() %
					Record->GetED2KPartHashCount() % Record->GetHashCount());
			return false;
		}
		const uint32 now = (uint32)time(NULL);
		const CMD4Hash &tkey = Record->GetFileHash();
		CKnownFileMap::iterator it = m_knownFileMap.find(tkey);
		if (it == m_knownFileMap.end()) {
			// Only stamp lastSeen=now for a confirmed sighting of the file on disk
			// (post-hash via CHashingTask, or any other afterHashing=true path). During
			// known.met load Append runs with afterHashing=false, and touching lastSeen
			// there would overwrite either the FT_LASTSEEN tag just loaded or the
			// m_lastDateChanged fallback CKnownFile::LoadFromFile substitutes when the
			// tag is absent. That fallback is the only thing letting the TTL prune do
			// useful migration work on an old known.met: trample it and every loaded
			// record looks fresh, so TTL never evicts anything.
			if (afterHashing) {
				Record->SetLastSeen(now);
				// Shared-since (issue #466): stamped once, and only for a genuinely
				// new file. Same afterHashing guard as lastSeen, so a known.met
				// load never re-stamps and a file predating the feature keeps 0
				// (unknown) rather than looking shared just now.
				if (Record->GetDateShared() == 0) {
					Record->SetDateShared(now);
				}
			}
			m_knownFileMap[tkey] = Record;
			InsertIntoSizeMap(m_knownSizeMap, Record);
			return true;
		} else {
			CKnownFile *existing = it->second;
			if (KnownFileMatches(Record,
				    existing->GetFileName(),
				    existing->GetLastChangeDatetime(),
				    existing->GetFileSize())) {
				// The file is already on the list, ignore it.
				AddDebugLogLineN(logKnownFiles,
					CFormat("%s is already on the list") %
						Record->GetFileName().GetPrintable());
				if (afterHashing) {
					existing->SetLastSeen(now);
				}
				return false;
			} else if (CKnownFile *dup = IsOnDuplicates(Record->GetFileName(),
					   Record->GetLastChangeDatetime(),
					   Record->GetFileSize())) {
				// The file is on the duplicates list, ignore it. Should not happen,
				// at least not after hashing -- why would it have been hashed?
				AddDebugLogLineN(logKnownFiles,
					CFormat("%s is on the duplicates list") %
						Record->GetFileName().GetPrintable());
				// Pin the duplicate only when this branch was reached by hashing a
				// real on-disk file. During load Append is not a fresh sighting,
				// and pinning would falsely protect stale records from the cap/TTL
				// prune.
				if (afterHashing) {
					dup->SetLastSeen(now);
					m_pinnedDuplicates.insert(dup);
				}
				return false;
			} else {
				if (afterHashing && existing->GetFileSize() == Record->GetFileSize()) {
					// We just hashed a "new" shared file and find it already
					// known under a different name or date: probably renamed or
					// touched. Copy over all properties from the existing known
					// file and keep only name/date.
					time_t newDate = Record->GetLastChangeDatetime();
					CPath newName = Record->GetFileName();
					CMemFile f;
					existing->WriteToFile(&f);
					f.Reset();
					if (!Record->LoadFromFile(&f)) {
						// this also shouldn't happen
						AddDebugLogLineC(logKnownFiles,
							CFormat("error copying known file: existing: %s %d "
								"%d %d  Record: %s %d %d %d") %
								existing->GetFileName().GetPrintable() %
								existing->GetFileSize() %
								existing->GetED2KPartHashCount() %
								existing->GetHashCount() %
								Record->GetFileName().GetPrintable() %
								Record->GetFileSize() %
								Record->GetED2KPartHashCount() %
								Record->GetHashCount());
						return false;
					}
					Record->SetLastChangeDatetime(newDate);
					Record->SetFileName(newName);
				}
				// The file is a duplicated hash. Add THE OLD ONE to the duplicates
				// list. (Used when reading the known file list, where the
				// duplicates are stored in front.)
				m_duplicateFileList.push_back(existing);
				InsertIntoSizeMap(m_duplicateSizeMap, existing);
				if (theApp->sharedfiles) {
					// Removing the old kad keywords created with the old filename
					theApp->sharedfiles->RemoveKeywords(existing);
				}
				// existing is leaving m_knownFileMap for m_duplicateFileList, so
				// drop its size-map entry or FindKnownFile returns a pointer that
				// no longer belongs to the live map.
				EraseFromSizeMap(m_knownSizeMap, existing);
				InsertIntoSizeMap(m_knownSizeMap, Record);
				// On the afterHashing path the copy-existing-tags block above
				// pulled the prior FT_LASTSEEN into Record, so refresh it or the
				// new live entry is born aged out of the TTL window. During load
				// keep Record's own loaded lastSeen: stamping the replacement as
				// fresh would make every live entry look load-time-fresh, and the
				// migration-driven TTL pass would never evict.
				if (afterHashing) {
					Record->SetLastSeen(now);
				}
				m_knownFileMap[tkey] = Record;
				return true;
			}
		}
	} else {
		AddDebugLogLineN(logGeneral, CFormat("%s is 0-size, not added") % Record->GetFileName());

		return false;
	}
}

// A (size, mtime) index to speed up FindKnownFile. Size plus mtime modulo 2^32
// is the same precision FindKnownFile's inputs and KnownFileMatches use.
void CKnownFileList::PrepareIndex()
{
	ReleaseIndex();
	m_knownSizeMap = new KnownFileSizeMap;
	for (const auto &entry : m_knownFileMap) {
		InsertIntoSizeMap(m_knownSizeMap, entry.second);
	}
	m_duplicateSizeMap = new KnownFileSizeMap;
	for (CKnownFile *record : m_duplicateFileList) {
		InsertIntoSizeMap(m_duplicateSizeMap, record);
	}
}

void CKnownFileList::ReleaseIndex()
{
	delete m_knownSizeMap;
	delete m_duplicateSizeMap;
	m_knownSizeMap = NULL;
	m_duplicateSizeMap = NULL;
}

void CKnownFileList::PruneDuplicates(const std::unordered_set<CKnownFile *> &inUse)
{
	// Caller must hold list_mut.

	// Gate on a full share-scan having run this session: before that inUse is empty and
	// FindKnownFile has not populated m_pinnedDuplicates, so a prune would drop records the
	// next scan would legitimately have pinned. Set by MarkInitialShareScanComplete() from
	// CSharedFileList::Reload.
	if (!m_initialShareScanComplete) {
		return;
	}

	const uint32 now = (uint32)time(NULL);
	const uint32 ttlCutoff = (now > KNOWN_DUPLICATE_TTL_SECS) ? (now - KNOWN_DUPLICATE_TTL_SECS) : 0;

	auto isProtected = [&](CKnownFile *r) {
		return inUse.count(r) > 0 || m_pinnedDuplicates.count(r) > 0;
	};

	// Pass 1: live entries past TTL. A non-refreshed live entry means no share-scan in the last
	// TTL window produced a (name, date, size) match, so the file is no longer accessible and
	// the whole hash is dead, duplicates included. std::set rather than unordered_set because
	// CMD4Hash provides operator< but no std::hash specialization.
	std::set<CMD4Hash> deadHashes;
	for (CKnownFileMap::const_iterator it = m_knownFileMap.begin(); it != m_knownFileMap.end(); ++it) {
		CKnownFile *live = it->second;
		if (isProtected(live)) {
			continue;
		}
		if (live->GetLastSeen() < ttlCutoff) {
			deadHashes.insert(it->first);
		}
	}

	// Pass 2: duplicates -- drop if their hash is dead, or their own
	// lastSeen is past TTL. Bucket survivors by hash for the cap pass.
	std::map<CMD4Hash, std::vector<KnownFileList::iterator>> survivors;
	size_t droppedDupTTL = 0;
	for (KnownFileList::iterator it = m_duplicateFileList.begin(); it != m_duplicateFileList.end();) {
		CKnownFile *record = *it;
		if (isProtected(record)) {
			survivors[record->GetFileHash()].push_back(it);
			++it;
			continue;
		}
		const bool hashDead = deadHashes.count(record->GetFileHash()) > 0;
		const bool ownStale = record->GetLastSeen() < ttlCutoff;
		if (hashDead || ownStale) {
			EraseFromSizeMap(m_duplicateSizeMap, record);
			KnownFileList::iterator victim = it++;
			Notify_KnownFileBeingDestroyed(record);
			delete record;
			m_duplicateFileList.erase(victim);
			++droppedDupTTL;
		} else {
			survivors[record->GetFileHash()].push_back(it);
			++it;
		}
	}

	// Pass 3: drop the dead live entries (and their size-map index).
	size_t droppedLive = 0;
	for (std::set<CMD4Hash>::const_iterator it = deadHashes.begin(); it != deadHashes.end(); ++it) {
		CKnownFileMap::iterator kit = m_knownFileMap.find(*it);
		if (kit == m_knownFileMap.end()) {
			continue;
		}
		CKnownFile *dead = kit->second;

		// Final re-check: Save() snapshots inUse under our own lock, but the snapshot's
		// sharedfiles / downloadqueue locks were released before the prune body ran, so a
		// concurrent SafeAddKFile or RemoveFile could have changed membership. Re-query
		// under the owner's lock immediately before the delete (#685).
		if (theApp && theApp->sharedfiles && theApp->sharedfiles->GetFileByID(*it) != NULL) {
			continue;
		}
		if (theApp && theApp->downloadqueue && theApp->downloadqueue->GetFileByID(*it) != NULL) {
			continue;
		}

		EraseFromSizeMap(m_knownSizeMap, dead);
		Notify_KnownFileBeingDestroyed(dead);
		delete dead;
		m_knownFileMap.erase(kit);
		++droppedLive;
	}

	// Pass 4: per-hash cap on whatever duplicate survivors remain.
	size_t droppedDupCap = 0;
	for (std::map<CMD4Hash, std::vector<KnownFileList::iterator>>::iterator bucket = survivors.begin();
		bucket != survivors.end();
		++bucket) {
		std::vector<KnownFileList::iterator> &iters = bucket->second;
		if (iters.size() <= KNOWN_DUPLICATE_HASH_CAP) {
			continue;
		}

		// Partition out protected entries first, so the cap counts only the
		// prunable remainder and protected records do not crowd out survivors.
		std::vector<KnownFileList::iterator> prunable;
		prunable.reserve(iters.size());
		for (size_t i = 0; i < iters.size(); ++i) {
			if (!isProtected(*iters[i])) {
				prunable.push_back(iters[i]);
			}
		}
		if (prunable.size() <= KNOWN_DUPLICATE_HASH_CAP) {
			continue;
		}

		// Newest mtime survives the cap: older mtimes for the same hash are
		// unlikely to match again, mtime being monotone-forward in practice.
		std::sort(prunable.begin(),
			prunable.end(),
			[](const KnownFileList::iterator &a, const KnownFileList::iterator &b) {
				return (*a)->GetLastChangeDatetime() > (*b)->GetLastChangeDatetime();
			});
		for (size_t i = KNOWN_DUPLICATE_HASH_CAP; i < prunable.size(); ++i) {
			CKnownFile *dead = *prunable[i];
			EraseFromSizeMap(m_duplicateSizeMap, dead);
			m_duplicateFileList.erase(prunable[i]);
			Notify_KnownFileBeingDestroyed(dead);
			delete dead;
			++droppedDupCap;
		}
	}

	if (droppedLive || droppedDupTTL || droppedDupCap) {
		AddDebugLogLineN(logKnownFiles,
			CFormat("known.met prune: dropped %u live + %u dup (TTL %u days) + %u dup (cap %u)") %
				(unsigned)droppedLive % (unsigned)droppedDupTTL %
				(unsigned)(KNOWN_DUPLICATE_TTL_SECS / (24 * 60 * 60)) %
				(unsigned)droppedDupCap % KNOWN_DUPLICATE_HASH_CAP);
	}
}

// File_checked_for_headers
