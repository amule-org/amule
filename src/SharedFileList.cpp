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

#include "SharedFileList.h" // Interface declarations  // Do_not_auto_remove
#include "SharedDirWatcher.h"

#include <map>
#include <unordered_set>

#include <protocol/Protocols.h>
#include <protocol/kad/Constants.h>
#include <tags/FileTags.h>

#include <wx/utils.h>

#include "Packet.h"           // Needed for CPacket
#include "MemFile.h"          // Needed for CMemFile
#include "ServerConnect.h"    // Needed for CServerConnect
#include "KnownFileList.h"    // Needed for CKnownFileList
#include "ThreadTasks.h"      // Needed for CThreadScheduler and CHasherTask
#include "MediaProbeThread.h" // Needed for CMediaProbeThread (media probe queue)
#include "OtherFunctions.h"   // Needed for GetED2KFileTypeID / ED2KFT_* (MaybeScheduleMediaProbe)
#include "ThreadScheduler.h"  // Needed for CThreadScheduler::GetPendingCount (bulk probe logging)
#include "Preferences.h"      // Needed for thePrefs
#include "DownloadQueue.h"    // Needed for CDownloadQueue
#include "amule.h"            // Needed for theApp
#include "PartFile.h"         // Needed for PartFile
#include "Server.h"           // Needed for CServer
#include "Statistics.h"       // Needed for theStats
#include "Logger.h"
#include <common/Format.h>
#include <common/FileFunctions.h>
#include "GuiEvents.h"  // Needed for Notify_*
#include "SHAHashSet.h" // Needed for CAICHHash

#include "kademlia/kademlia/Kademlia.h"
#include "kademlia/kademlia/Search.h"
#include "ClientList.h"

#include <vector>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

typedef std::deque<CKnownFile *> KnownFileArray;

// m_pathIndex key canonicalization. macOS hands the same filename to different
// subsystems in different Unicode normalization forms: known.met / download
// names arrive decomposed (NFD) while FSEvents can report the very same path
// composed (NFC), and a file's CREATE and DELETE events may even disagree. A
// plain map keyed on the raw path string therefore misses when the lookup form
// differs from the stored form, so a completed download with an accented name
// (e.g. "corazón") never auto-unshares on delete. Fold every key to NFC so all
// variants of one name collapse to a single entry. Off macOS filenames are
// opaque byte strings with no OS-level normalization, so this is identity.
static wxString NormalizePathKey(const wxString &path)
{
#ifdef __APPLE__
	const wxScopedCharBuffer utf8 = path.utf8_str();
	if (utf8.data() == NULL || utf8.length() == 0) {
		return path;
	}
	CFStringRef s = CFStringCreateWithBytes(kCFAllocatorDefault,
		(const UInt8 *)utf8.data(),
		(CFIndex)utf8.length(),
		kCFStringEncodingUTF8,
		false);
	if (s == NULL) {
		return path;
	}
	CFMutableStringRef mut = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, s);
	CFRelease(s);
	if (mut == NULL) {
		return path;
	}
	CFStringNormalize(mut, kCFStringNormalizationFormC);
	const CFIndex maxLen =
		CFStringGetMaximumSizeForEncoding(CFStringGetLength(mut), kCFStringEncodingUTF8) + 1;
	std::vector<char> buf((size_t)maxLen);
	const Boolean ok = CFStringGetCString(mut, buf.data(), maxLen, kCFStringEncodingUTF8);
	CFRelease(mut);
	return ok ? wxString::FromUTF8(buf.data()) : path;
#else
	return path;
#endif
}

///////////////////////////////////////////////////////////////////////////////
// CPublishKeyword

class CPublishKeyword
{
public:
	CPublishKeyword(const wxString &rstrKeyword)
	{
		m_strKeyword = rstrKeyword;
		// min. keyword char is allowed to be < 3 in some cases (see also 'CSearchManager::getWords')
		// ASSERT( rstrKeyword.GetLength() >= 3 );
		wxASSERT(!rstrKeyword.IsEmpty());
		KadGetKeywordHash(rstrKeyword, &m_nKadID);
		SetNextPublishTime(0);
		SetPublishedCount(0);
	}

	const Kademlia::CUInt128 &GetKadID() const { return m_nKadID; }
	const wxString &GetKeyword() const { return m_strKeyword; }
	int GetRefCount() const { return m_aFiles.size(); }
	const KnownFileArray &GetReferences() const { return m_aFiles; }

	uint32 GetNextPublishTime() const { return m_tNextPublishTime; }
	void SetNextPublishTime(uint32 tNextPublishTime) { m_tNextPublishTime = tNextPublishTime; }

	uint32 GetPublishedCount() const { return m_uPublishedCount; }
	void SetPublishedCount(uint32 uPublishedCount) { m_uPublishedCount = uPublishedCount; }
	void IncPublishedCount() { m_uPublishedCount++; }

	bool AddRef(CKnownFile *pFile)
	{
		if (std::find(m_aFiles.begin(), m_aFiles.end(), pFile) != m_aFiles.end()) {
			wxFAIL;
			return false;
		}
		m_aFiles.push_back(pFile);
		return true;
	}

	int RemoveRef(CKnownFile *pFile)
	{
		KnownFileArray::iterator it = std::find(m_aFiles.begin(), m_aFiles.end(), pFile);
		if (it != m_aFiles.end()) {
			m_aFiles.erase(it);
		}
		return m_aFiles.size();
	}

	void RemoveAllReferences() { m_aFiles.clear(); }

	void RotateReferences(unsigned iRotateSize)
	{
		wxCHECK_RET(m_aFiles.size(), "RotateReferences: Rotating empty array");

		unsigned shift = (iRotateSize % m_aFiles.size());
		std::rotate(m_aFiles.begin(), m_aFiles.begin() + shift, m_aFiles.end());
	}

protected:
	wxString m_strKeyword;
	Kademlia::CUInt128 m_nKadID;
	uint32 m_tNextPublishTime;
	uint32 m_uPublishedCount;
	KnownFileArray m_aFiles;
};

///////////////////////////////////////////////////////////////////////////////
// CPublishKeywordList

class CPublishKeywordList
{
public:
	CPublishKeywordList();
	~CPublishKeywordList();

	void AddKeyword(const wxString &keyword, CKnownFile *file);
	void AddKeywords(CKnownFile *pFile);
	void RemoveKeyword(const wxString &keyword, CKnownFile *file);
	void RemoveKeywords(CKnownFile *pFile);
	void RemoveAllKeywords();

	void RemoveAllKeywordReferences();
	void PurgeUnreferencedKeywords();

	int GetCount() const { return m_lstKeywords.size(); }

	CPublishKeyword *GetNextKeyword();
	void ResetNextKeyword();

	uint32 GetNextPublishTime() const { return m_tNextPublishKeywordTime; }
	void SetNextPublishTime(uint32 tNextPublishKeywordTime)
	{
		m_tNextPublishKeywordTime = tNextPublishKeywordTime;
	}

protected:
	// The list is the canonical container — its insertion order is
	// load-bearing for GetNextKeyword()'s round-robin publish cursor
	// (m_posNextKeyword), so we cannot replace it with a map.
	typedef std::list<CPublishKeyword *> CKeyWordList;
	CKeyWordList m_lstKeywords;
	CKeyWordList::iterator m_posNextKeyword;
	uint32 m_tNextPublishKeywordTime;

	// Secondary index: keyword string -> position in m_lstKeywords. Lets
	// FindKeyword() do an O(log N) lookup instead of a linear scan,
	// collapsing AddKeywords()'s hot path on large shared sets
	// (CSharedFileList::Reload, called once per shared file at startup)
	// from O(N²) to O(N log N). std::list iterators are stable across
	// other inserts/erases, so caching them here is safe.
	std::map<wxString, CKeyWordList::iterator> m_keywordIndex;

	CPublishKeyword *FindKeyword(const wxString &rstrKeyword, CKeyWordList::iterator *ppos = NULL);
};

CPublishKeywordList::CPublishKeywordList()
{
	ResetNextKeyword();
	SetNextPublishTime(0);
}

CPublishKeywordList::~CPublishKeywordList()
{
	RemoveAllKeywords();
}

CPublishKeyword *CPublishKeywordList::GetNextKeyword()
{
	if (m_posNextKeyword == m_lstKeywords.end()) {
		m_posNextKeyword = m_lstKeywords.begin();
		if (m_posNextKeyword == m_lstKeywords.end()) {
			return NULL;
		}
	}
	return *m_posNextKeyword++;
}

void CPublishKeywordList::ResetNextKeyword()
{
	m_posNextKeyword = m_lstKeywords.begin();
}

CPublishKeyword *CPublishKeywordList::FindKeyword(const wxString &rstrKeyword, CKeyWordList::iterator *ppos)
{
	std::map<wxString, CKeyWordList::iterator>::iterator idx = m_keywordIndex.find(rstrKeyword);
	if (idx == m_keywordIndex.end()) {
		return NULL;
	}
	if (ppos) {
		*ppos = idx->second;
	}
	return *(idx->second);
}

void CPublishKeywordList::AddKeyword(const wxString &keyword, CKnownFile *file)
{
	CPublishKeyword *pubKw = FindKeyword(keyword);
	if (pubKw == NULL) {
		pubKw = new CPublishKeyword(keyword);
		m_lstKeywords.push_back(pubKw);
		CKeyWordList::iterator it = m_lstKeywords.end();
		--it;
		m_keywordIndex[keyword] = it;
		SetNextPublishTime(0);
	}
	pubKw->AddRef(file);
}

void CPublishKeywordList::AddKeywords(CKnownFile *pFile)
{
	const Kademlia::WordList &wordlist = pFile->GetKadKeywords();

	Kademlia::WordList::const_iterator it;
	for (it = wordlist.begin(); it != wordlist.end(); ++it) {
		AddKeyword(*it, pFile);
	}
}

void CPublishKeywordList::RemoveKeyword(const wxString &keyword, CKnownFile *file)
{
	CKeyWordList::iterator pos;
	CPublishKeyword *pubKw = FindKeyword(keyword, &pos);
	if (pubKw != NULL) {
		if (pubKw->RemoveRef(file) == 0) {
			if (pos == m_posNextKeyword) {
				++m_posNextKeyword;
			}
			m_lstKeywords.erase(pos);
			m_keywordIndex.erase(keyword);
			delete pubKw;
			SetNextPublishTime(0);
		}
	}
}

void CPublishKeywordList::RemoveKeywords(CKnownFile *pFile)
{
	const Kademlia::WordList &wordlist = pFile->GetKadKeywords();
	Kademlia::WordList::const_iterator it;
	for (it = wordlist.begin(); it != wordlist.end(); ++it) {
		RemoveKeyword(*it, pFile);
	}
}

void CPublishKeywordList::RemoveAllKeywords()
{
	DeleteContents(m_lstKeywords);
	m_keywordIndex.clear();
	ResetNextKeyword();
	SetNextPublishTime(0);
}

void CPublishKeywordList::RemoveAllKeywordReferences()
{
	CKeyWordList::iterator it = m_lstKeywords.begin();
	for (; it != m_lstKeywords.end(); ++it) {
		(*it)->RemoveAllReferences();
	}
}

void CPublishKeywordList::PurgeUnreferencedKeywords()
{
	CKeyWordList::iterator it = m_lstKeywords.begin();
	while (it != m_lstKeywords.end()) {
		CPublishKeyword *pPubKw = *it;
		if (pPubKw->GetRefCount() == 0) {
			if (it == m_posNextKeyword) {
				++m_posNextKeyword;
			}
			m_keywordIndex.erase(pPubKw->GetKeyword());
			m_lstKeywords.erase(it++);
			delete pPubKw;
			SetNextPublishTime(0);
		} else {
			++it;
		}
	}
}

CSharedFileList::CSharedFileList(CKnownFileList *in_filelist)
{
	filelist = in_filelist;
	reloading = false;
	m_lastPublishED2K = 0;
	m_lastPublishED2KFlag = true;
	/* Kad Stuff */
	m_keywords = new CPublishKeywordList;
	m_currFileSrc = 0;
	m_currFileNotes = 0;
	m_lastPublishKadSrc = 0;
	m_lastPublishKadNotes = 0;
	m_currFileKey = 0;
	m_dirWatcher = NULL;
}

CSharedFileList::~CSharedFileList()
{
	delete m_dirWatcher;
	delete m_keywords;
}

void CSharedFileList::EnableDirectoryWatcher(bool enable)
{
	if (enable) {
		if (!m_dirWatcher) {
			m_dirWatcher = new CSharedDirWatcher(this);
		}
		m_dirWatcher->Enable();
	} else if (m_dirWatcher) {
		m_dirWatcher->Disable();
	}
}

void CSharedFileList::FindSharedFiles(const ReloadYieldCb &yieldCb, bool &aborted)
{
	/* Abort loading if we are shutting down. */
	if (theApp->IsOnShutDown()) {
		return;
	}

	// Clear statistics.
	theStats::ClearSharedFilesInfo();

	// Reload shareddir.dat
	theApp->glob_prefs->ReloadSharedFolders();

	{
		wxMutexLocker lock(list_mut);
		m_Files_map.clear();
		// The index goes with the map it mirrors. AddFile writes a key only on
		// a fresh insert and RemoveFile erases only the one key it recomputes,
		// so any path this walk does not re-add -- a file deleted while its
		// root was unshared, a DELETE lost to a watcher backend overflow, a
		// remote deletion on a network share -- would keep an entry that makes
		// NotifyPathAdded treat the path as already shared and silently refuse
		// to share it again, with nothing logged at any level. Clearing here
		// cannot heal that on its own; leaving the key behind is what creates
		// it (issue #1028).
		//
		// Same locked scope on purpose: this puts the index under the invariant
		// the map already has -- neither may be observed between here and the
		// end of the walk. That is not a new constraint, it is the one that
		// already rules out pumping the event loop during a walk (see
		// SharedFilesReloadProgress.h). Keeping the two containers on one rule
		// is why they are cleared together rather than separately.
		m_pathIndex.clear();
		m_listGeneration.fetch_add(1, std::memory_order_relaxed);
	}

	// All part files are automatically shared.
	for (uint32 i = 0; i < theApp->downloadqueue->GetFileCount(); ++i) {
		CPartFile *file = theApp->downloadqueue->GetFileByIndex(i);

		if (file->GetStatus(true) == PS_READY) {
			AddLogLineNS(
				CFormat(_("Adding file %s to shares")) % file->GetFullName().GetPrintable());
			AddFile(file);
		}
	}

	// Create a list of all shared paths and weed out duplicates.
	std::list<CPath> sharedPaths;

	// Global incoming dir and all category incoming directories are automatically shared.
	sharedPaths.push_back(thePrefs::GetIncomingDir());
	for (unsigned int i = 1; i < theApp->glob_prefs->GetCatCount(); ++i) {
		sharedPaths.push_back(theApp->glob_prefs->GetCatPath(i));
	}

	const thePrefs::PathList &shared = theApp->glob_prefs->shareddir_list;
	sharedPaths.insert(sharedPaths.end(), shared.begin(), shared.end());

	sharedPaths.sort();
	sharedPaths.unique();

	filelist->PrepareIndex();
	// Gathering is done in the foreground and can be slowed down severely by parallel background hashing.
	// So just store the hashing tasks for now.
	TaskList hashTasks;
	size_t scanned = 0;
	size_t excluded = 0;
	for (std::list<CPath>::iterator it = sharedPaths.begin(); it != sharedPaths.end(); ++it) {
		AddFilesFromDirectory(*it, hashTasks, yieldCb, scanned, excluded, aborted);
		if (aborted) {
			break;
		}
	}
	filelist->ReleaseIndex();

	// Now that the shared files are gathered feed the hashing tasks to the scheduler to start hashing.
	unsigned addedFiles = 0;
	for (TaskList::iterator it = hashTasks.begin(); it != hashTasks.end(); ++it) {
		if (CThreadScheduler::AddTask(*it)) {
			addedFiles++;
		}
	}
	// Accepted tasks only, matching what the old "%i unknown" suffix counted.
	m_discoveredNewFiles += addedFiles;

	// One unconditional summary. The new-file count used to ride along as a
	// suffix here, in one of two mutually exclusive variants, and only for this
	// route -- the watcher reported nothing at all. It now has its own line,
	// emitted from Process() for both routes (issue #968), so this says one
	// thing and the two-argument variant is retired. That also drops a msgid
	// whose plural form was selected on GetCount() rather than on the count it
	// was actually pluralising.
	AddLogLineN(
		CFormat(wxPLURAL("Found %i known shared file", "Found %i known shared files", GetCount())) %
		GetCount());

	if (addedFiles == 0) {
		// Make sure the AICH-hashes are up to date. This is the startup sync
		// run once the shared/known list is authoritative, so it opts into the
		// orphan-prune (drop known2_64.met entries no longer owned by any known
		// file). Post-hashing syncs deliberately do not prune -- see
		// CAICHSyncTask's ctor doc.
		//
		// Unchanged condition: this scheduling has nothing to do with logging
		// and must keep firing exactly when it did before.
		CThreadScheduler::AddTask(new CAICHSyncTask(true));
	}

	if (excluded > 0) {
		AddLogLineN(CFormat(wxPLURAL("Excluded %i file from sharing by filter",
				    "Excluded %i files from sharing by filter",
				    excluded)) %
			    excluded);
	}
}

// Checks if the dir a is the same as b. If they are, then logs the message and returns true.
static bool CheckDirectory(const wxString &a, const CPath &b)
{
	if (CPath(a).IsSameDir(b)) {
		AddLogLineC(CFormat(_("ERROR: Attempted to share %s")) % a);

		return true;
	}

	return false;
}

unsigned CSharedFileList::AddFilesFromDirectory(const CPath &directory,
	TaskList &hashTasks,
	const ReloadYieldCb &yieldCb,
	size_t &scanned,
	size_t &excluded,
	bool &aborted)
{
	// Do not allow these folders to be shared:
	//  - The .aMule folder
	//  - The Temp folder
	//  - The users home-dir
	if (CheckDirectory(wxGetHomeDir(), directory)) {
		return 0;
	} else if (CheckDirectory(thePrefs::GetConfigDir(), directory)) {
		return 0;
	} else if (CheckDirectory(thePrefs::GetTempDir().GetRaw(), directory)) {
		return 0;
	}

	if (!directory.DirExists()) {
		AddLogLineNS(
			CFormat(_("Shared directory not found, skipping: %s")) % directory.GetPrintable());

		return 0;
	}

	CDirIterator::FileType searchFor = CDirIterator::FileNoHidden;
	if (thePrefs::ShareHiddenFiles()) {
		searchFor = CDirIterator::File;
	}

	const int extraFlags = thePrefs::FollowSymlinksInShares() ? 0 : wxDIR_NO_FOLLOW;

	unsigned knownFiles = 0;
	unsigned addedFiles = 0;

	// Yield to the caller every kYieldEvery files so the UI can stay
	// responsive on big shared trees. 256 strikes a balance between
	// progress-bar responsiveness (~4 updates/s on a 1 ms-per-file
	// machine) and the overhead of the callback itself.
	constexpr size_t kYieldEvery = 256;

	CDirIterator SharedDir(directory);

	for (CPath fname = SharedDir.GetFirstFile(searchFor, wxEmptyString, extraFlags); fname.IsOk();
		fname = SharedDir.GetNextFile()) {
		if (yieldCb && ++scanned % kYieldEvery == 0) {
			if (!yieldCb(scanned)) {
				aborted = true;
				return addedFiles;
			}
		} else if (!yieldCb) {
			++scanned;
		}

		switch (AddPathToShares(directory, fname, hashTasks)) {
		case kAddPathQueued:
			addedFiles++;
			break;
		case kAddPathKnown:
		case kAddPathAlreadyShared:
			// Both are "known" for the summary count; only the attach differs.
			knownFiles++;
			break;
		case kAddPathExcluded:
			excluded++;
			break;
		case kAddPathSkipped:
			break;
		}
	}

	if ((addedFiles == 0) && (knownFiles == 0)) {
		AddLogLineN(
			CFormat(_("No shareable files found in directory: %s")) % directory.GetPrintable());
	}

	return addedFiles;
}

// Per-path attach. Three outcomes:
//   kAddPathSkipped — broken link, zero size, stat failed; do nothing.
//   kAddPathKnown   — matched a CKnownFile in known.met and was either
//                     newly attached to the shared list or already there.
//   kAddPathQueued  — unknown file; a CHashingTask was pushed into
//                     hashTasks. The shared-list attach happens later
//                     when the hashing thread finishes and calls
//                     SafeAddKFile() on the resulting CKnownFile.
//
// Shared between the bulk directory walk (AddFilesFromDirectory above)
// and the incremental watcher path (NotifyPathAdded below) so the two
// agree on shareability rules.
CSharedFileList::AddPathResult CSharedFileList::AddPathToShares(
	const CPath &directory, const CPath &fname, TaskList &hashTasks, bool notifyGuiOnKnownAdd)
{
	CPath fullPath = directory.JoinPaths(fname);

	if (!fullPath.FileExists()) {
		AddDebugLogLineN(logKnownFiles,
			CFormat("Shared file does not exist (possibly a broken link): %s") % fullPath);
		return kAddPathSkipped;
	}

	AddDebugLogLineN(logKnownFiles, CFormat("Found shared file: %s") % fullPath);

	// User-configured name exclusion. Checked before the stat calls below
	// so excluded files cost only a name match. Applies identically to the
	// bulk walk and the incremental watcher path.
	if (thePrefs::IsShareExcluded(fname.GetPrintable())) {
		AddDebugLogLineN(logKnownFiles, CFormat("Excluded from shares by filter: %s") % fullPath);
		return kAddPathExcluded;
	}

	time_t fdate = CPath::GetModificationTime(fullPath);
	sint64 fsize = fullPath.GetFileSize();

	// This will also catch files with too strict permissions.
	if ((fdate == (time_t)-1) || (fsize == wxInvalidOffset)) {
		AddDebugLogLineN(logKnownFiles,
			CFormat("Failed to retrieve modification time or size for '%s', skipping.") %
				fullPath);
		return kAddPathSkipped;
	}

	if (fsize == 0) {
		AddDebugLogLineN(logKnownFiles, CFormat("Skip zero size file '%s'") % fullPath);
		return kAddPathSkipped;
	}

	CKnownFile *toadd = filelist->FindKnownFile(fname, fdate, fsize);
	if (toadd) {
		// Ask before stamping. SetFilePath is not a plain assignment -- it
		// calls MarkECChanged(), which hands the file a new EC generation and
		// so pushes it into the next INC_UPDATE. Stamping and then rolling
		// back on a decline did that twice for a net-zero path change, sending
		// every duplicate-content file to every EC client as "changed" on each
		// reload, which is precisely what the generation counter exists to
		// avoid (issue #1028).
		//
		// Note the obvious guard -- stamp only when the path differs -- would
		// not have helped: a decline with previousPath == directory is close to
		// unreachable, because the watcher route returns on an index hit before
		// reaching here and the bulk walk clears the map up front and visits
		// each root once. Every decline that happens in practice has a
		// different path, so that guard skips nothing.
		{
			wxMutexLocker lock(list_mut);
			if (m_Files_map.find(toadd->GetFileHash()) != m_Files_map.end()) {
				AddDebugLogLineN(
					logKnownFiles, CFormat("File already shared, skipping: %s") % fname);
				return kAddPathAlreadyShared;
			}
		}

		// Set the path BEFORE AddFile so the path index that AddFile
		// maintains keys off the file's current GetFilePath() rather
		// than whatever stale path was stamped on the CKnownFile by
		// a previous shared-list membership.
		//
		// The rollback below still stands as the fallback for the
		// check-then-insert race: the membership test above drops the lock
		// before AddFile retakes it, so a hashing task completing through
		// SafeAddKFile in between can still make AddFile decline.
		const CPath previousPath = toadd->GetFilePath();
		toadd->SetFilePath(directory);
		if (AddFile(toadd)) {
			AddDebugLogLineN(logKnownFiles, CFormat("Added known file '%s' to shares") % fname);
			// This record matched a file we just saw on disk and it now
			// owns the hash in the shared list. Give it the hash in the
			// known-file map too: that map keeps whichever known.met
			// entry loaded last, which for duplicated content can be a
			// different record -- and if that record's own copy has been
			// deleted, every hash-keyed known-file lookup resolves to a
			// path that cannot be opened (issue #1265). Called outside
			// AddFile's lock on purpose: nothing may enter knownfiles
			// while holding the shared-list lock.
			filelist->PromoteToCanonical(toadd);
			// The bulk-Reload caller repaints the whole view once its
			// walk finishes; the incremental watcher caller has no such
			// follow-up, so tell the GUI about this freshly-shared file
			// directly (otherwise it stays invisible in the shared-files
			// view despite being in the core share set -- see the header).
			if (notifyGuiOnKnownAdd) {
				Notify_SharedFilesShowFile(toadd);
			}
		} else {
			// The share set already holds this content, indexed under the
			// path it was first found at. Put the file back on that path:
			// the stamp above would otherwise leave GetFilePath() disagreeing
			// with the index key, and nothing reconciles them. That skew is
			// not cosmetic -- RemoveFile erases by a key it recomputes from
			// GetFilePath(), so the erase silently misses and leaks the real
			// entry, after which NotifyPathAdded short-circuits on the leaked
			// key and the file can never be shared again without a restart.
			// The upload worker also opens GetFilePath(), so it would read
			// the copy the index does not know about and, on failure, invoke
			// its own "removing from list of shared files" recovery against a
			// file that is still present and serveable (issue #1017).
			//
			// First copy found wins and stays authoritative, which is stable
			// across reloads. Re-keying the index to follow the new path would
			// work too, but would make "last directory walked wins" the
			// semantics, so which physical copy serves uploads would depend on
			// the sort order of the shared roots.
			toadd->SetFilePath(previousPath);
			AddDebugLogLineN(logKnownFiles, CFormat("File already shared, skipping: %s") % fname);
			return kAddPathAlreadyShared;
		}
		return kAddPathKnown;
	}

	// Not in knownfilelist - start adding thread to hash file.
	AddDebugLogLineN(logKnownFiles, CFormat("Hashing new unknown shared file '%s'") % fname);

	hashTasks.push_back(new CHashingTask(directory, fname));
	// Not counted here. CThreadScheduler::DoAddTask dedups on (type, desc) and
	// silently drops a task already queued, so a re-walk while hashing is still
	// pending constructs the same tasks again -- counting at construction would
	// report those files as discovered twice. Both routes count where the
	// scheduler actually accepts the task instead.
	return kAddPathQueued;
}

bool CSharedFileList::AddFile(CKnownFile *pFile)
{
	wxASSERT(pFile->GetHashCount() == pFile->GetED2KPartHashCount());

	wxMutexLocker lock(list_mut);

	CKnownFileMap::value_type entry(pFile->GetFileHash(), pFile);
	if (m_Files_map.insert(entry).second) {
		m_listGeneration.fetch_add(1, std::memory_order_relaxed);
		/* Keywords to publish on Kad */
		m_keywords->AddKeywords(pFile);
		theStats::AddSharedFile(pFile->GetFileSize());
		// Mirror into the path index so the watcher's per-event
		// dispatch can resolve DELETE / MODIFY events to the
		// CKnownFile* in O(1). Empty key (e.g. a CPartFile whose
		// SetFilePath has not run yet) is harmless: it lives in
		// m_pathIndex under "" until SafeAddKFile attaches the real
		// path via the post-completion path. Stale entries left
		// over from a previous shared-list membership are
		// overwritten here.
		const wxString key =
			NormalizePathKey(pFile->GetFilePath().JoinPaths(pFile->GetFileName()).GetRaw());
		m_pathIndex[key] = pFile;
		// Two ways this is a mass operation rather than one file the user is
		// watching. `reloading` covers the share walk itself. The hashing
		// queue covers what the walk leaves behind: files it discovered are
		// hashed asynchronously and only reach here when their task finishes,
		// by which time the walk is long over -- so a first import of a large
		// library would print one line per file with `reloading` alone, which
		// is precisely the log flood the summary exists to prevent.
		//
		// `> 1`, not `> 0`: GetPendingCount counts the running task too, so a
		// single file dropped into a shared directory reads as 1 while it is
		// being hashed. Misjudging the last file of an import as singular
		// costs one extra line; misjudging a single file as bulk would lose
		// the only feedback that file ever produces.
		const bool massOperation = reloading || CThreadScheduler::GetPendingCount(wxT("Hashing")) > 1;
		MaybeScheduleMediaProbe(pFile, MediaProbeMode::Normal, massOperation);
		return true;
	}
	return false;
}

bool CSharedFileList::MaybeScheduleMediaProbe(CKnownFile *pFile, MediaProbeMode mode, bool bulk)
{
	// Callers must keep pFile alive for the duration of this call. AddFile
	// holds list_mut, which does that; the refresh walk instead snapshots the
	// pointers under the lock and schedules outside it, which is safe because
	// both callers run on the main thread and only the main thread removes a
	// file from the list. It is that single-thread property doing the work
	// here, not the mutex.
	// #140 — probe local shared audio / video files with ffprobe to populate
	// the six FT_MEDIA_* fields (length, bitrate, codec, artist, album,
	// title). Cost-limiting:
	//  * on by default since #1080, and still switchable off in Preferences,
	//  * only for files whose ED2K file-type is audio / video (cheap
	//    extension-based filter — a .zip renamed to .mp4 gets
	//    scheduled and ffprobe fails fast in the worker, but this
	//    filter skips the mass of docs / archives / images in a
	//    typical share tree),
	//  * only when the file has no media metadata at all yet
	//    (GetMetaDataVer() == 0), so a probed file is never re-probed.
	// An empty ffprobe path is NOT "off": it means auto-detect.
	// CThreadScheduler naturally throttles: it runs one task at a
	// time at ETP_Low so hashing / completion never starve.
	if (!thePrefs::GetMediaMetadataEnabled()) {
		return false;
	}
	// An empty preference is not "off" -- it means "whatever this machine
	// has", which is what every place that documents the setting promises.
	// The worker resolves it through MediaProbe::DetectedPath() and drops
	// the job if that finds nothing; doing it there keeps the detection
	// subprocess off this thread and pays for it once per process rather
	// than once per file.
	const wxString &ffprobePath = thePrefs::GetMediaMetadataFFProbePath();
	// Shared with the GUI's menu-enable test (IsMediaProbeCandidate, in
	// OtherFunctions): the view must not offer an action the scheduler will
	// silently drop, which is what two copies of this rule would eventually
	// produce.
	if (!IsMediaProbeCandidate(pFile->GetFileName())) {
		return false;
	}
	// Never probe an in-progress download. A partfile is shared while
	// transferring, so this fires from AddFile() during the download; there is
	// no complete file to read yet (its on-disk name is <hash>.part), and its
	// metadata is derived exactly once -- on completion, which re-enters here
	// with bForceReprobe set. Skipping unconditionally (not just when metadata
	// happened to be inherited from the search result) keeps that guarantee.
	// Only Completion lifts this, and only because a just-completed download
	// is still a CPartFile object while its bytes are already all on disk. A
	// Refresh walk MUST NOT inherit that licence: an in-progress download is
	// in the shared list too, and there is nothing complete to read for it.
	if (mode != MediaProbeMode::Completion && pFile->IsPartFile()) {
		AddDebugLogLineN(logMediaProbe,
			CFormat(wxT("MediaProbe: skip (incomplete download) %s")) % pFile->GetFileName());
		return false;
	}
	// GetMetaDataVer(), not a second FT_MEDIA_LENGTH test: one definition of
	// "this file has been probed", shared with the publishers and the UI. The
	// length-only form here never considered a codec-only file probed, so
	// every startup re-ran ffprobe on all of them.
	if (mode == MediaProbeMode::Normal && pFile->GetMetaDataVer() > 0) {
		AddDebugLogLineN(logMediaProbe,
			CFormat(wxT("MediaProbe: skip (already has metadata) %s")) % pFile->GetFileName());
		return false;
	}
	// A file ffprobe already tried and could not read is not re-tried on the
	// normal path. Nothing about it has changed since the last attempt, so a
	// share reload would spawn ffprobe on every broken file in the library,
	// every time, to fail identically (issue #1116). Refresh deliberately
	// ignores this -- that is the whole point of an explicit re-extraction --
	// and a successful probe clears the marker.
	if (mode == MediaProbeMode::Normal && pFile->GetIntTagValue(FT_MEDIA_PROBE_FAILED)) {
		AddDebugLogLineN(logMediaProbe,
			CFormat(wxT("MediaProbe: skip (previous probe found nothing) %s")) %
				pFile->GetFileName());
		return false;
	}
	const CPath fullPath = pFile->GetFilePath().JoinPaths(pFile->GetFileName());
	// Only probe a file that is actually on disk at this resolved path -- a
	// stale known.met record can outlive its deleted file, and this is cheap
	// insurance against handing ffprobe a path that cannot succeed. (In-progress
	// downloads are already excluded by the partfile guard above.)
	//
	// Skipped for Refresh, which walks the WHOLE share from an EC handler on
	// the main thread: one stat per file is cheap, N of them synchronously
	// before the reply goes out is not, and it is the same stall the shared-
	// files reload was deliberately made asynchronous to avoid. Nothing is
	// lost by deferring it -- MediaProbe::Probe stats the path again on the
	// worker before spawning ffprobe, and logs the file as vanished. The cost
	// is that `queued` counts a file that has since been deleted, which is
	// honest: it says how many were accepted for probing, not how many
	// produced metadata.
	if (mode != MediaProbeMode::Refresh && !fullPath.FileExists()) {
		return false;
	}
	// #280: run on the dedicated media-probe worker, NOT the shared
	// CThreadScheduler — a slow/hung ffprobe there wedges completions.
	if (theApp->mediaProbeThread) {
		AddDebugLogLineN(logMediaProbe,
			CFormat(wxT("MediaProbe: queueing %s (ffprobe=%s)")) % pFile->GetFileName() %
				ffprobePath);
		theApp->mediaProbeThread->QueueProbe(pFile->GetFileHash(), fullPath, ffprobePath, bulk);
		return true;
	} else {
		AddDebugLogLineN(logMediaProbe,
			CFormat(wxT("MediaProbe: dropped %s - probe thread not ready")) %
				pFile->GetFileName());
	}
	return false;
}

// A user-triggered refresh must never decline in silence. The scheduler's own
// "feature is off" check is a debug line, which compiles out of release builds,
// so a refresh with media metadata disabled did exactly nothing and said
// nothing -- the GUI greys its entry out for this now, but an older GUI, a
// script driving EC, or the REST endpoint can still ask.
static bool MediaRefreshAvailable()
{
	if (thePrefs::GetMediaMetadataEnabled()) {
		return true;
	}
	AddLogLineN(_("Media metadata extraction is disabled in preferences, so there is nothing to "
		      "re-extract."));
	return false;
}

unsigned CSharedFileList::RefreshAllMediaMetadata()
{
	if (!MediaRefreshAvailable()) {
		return 0;
	}
	// Snapshot under the lock, schedule outside it. Holding list_mut across a
	// whole library's worth of scheduling would block every reader, including
	// the EC handlers this is invoked from.
	std::vector<CKnownFile *> files;
	{
		wxMutexLocker lock(list_mut);
		files.reserve(m_Files_map.size());
		for (const auto &entry : m_Files_map) {
			files.push_back(entry.second);
		}
	}

	unsigned queued = 0;
	for (CKnownFile *file : files) {
		if (MaybeScheduleMediaProbe(file, MediaProbeMode::Refresh, /*bulk=*/true)) {
			++queued;
		}
	}
	AddLogLineN(CFormat(wxPLURAL("Re-extracting media metadata for %u shared file",
			    "Re-extracting media metadata for %u shared files",
			    queued)) %
		    queued);
	return queued;
}

unsigned CSharedFileList::RefreshMediaMetadata(const std::vector<CMD4Hash> &hashes)
{
	if (!MediaRefreshAvailable()) {
		return 0;
	}
	unsigned queued = 0;
	for (const CMD4Hash &hash : hashes) {
		CKnownFile *file = GetFileByID(hash);
		// bulk: a selection of many files is one user action and reports one
		// summary, the same as a whole-share refresh. A selection of one is a
		// single file the user is looking at, and keeps its per-file line.
		if (file && MaybeScheduleMediaProbe(file, MediaProbeMode::Refresh, hashes.size() > 1)) {
			++queued;
		}
	}
	return queued;
}

bool CSharedFileList::RefreshMediaMetadata(const CMD4Hash &hash)
{
	if (!MediaRefreshAvailable()) {
		return false;
	}
	CKnownFile *file = GetFileByID(hash);
	return file && MaybeScheduleMediaProbe(file, MediaProbeMode::Refresh);
}

void CSharedFileList::SafeAddKFile(CKnownFile *toadd, bool bOnlyAdd)
{
	// Straight insert first. If the hash isn't already in m_Files_map
	// this succeeds and fires the notifier as before.
	if (AddFile(toadd)) {
		Notify_SharedFilesShowFile(toadd);
	} else {
		// AddFile failed because some CKnownFile under this hash is
		// already in m_Files_map. Two possibilities:
		//
		//   1. The exact same pointer was re-added — no-op.
		//   2. CKnownFileList::Append fired the rename-during-hash
		//      branch (same hash, same size, different name): it
		//      demoted the prior CKnownFile to m_duplicateFileList and
		//      installed `toadd` as the canonical entry in
		//      m_knownFileMap. The shared-files view still points at
		//      the demoted pointer, which has a filename that no
		//      longer matches disk and which the duplicate-list prune
		//      may delete later (dangling pointer in m_Files_map /
		//      m_pathIndex). Detach the stale entry and install the
		//      live one so the view mirrors knownfiles.
		CKnownFile *stale = NULL;
		bool alreadyCanonical = false;
		{
			wxMutexLocker lock(list_mut);
			CKnownFileMap::iterator it = m_Files_map.find(toadd->GetFileHash());
			if (it != m_Files_map.end()) {
				if (it->second != toadd) {
					stale = it->second;
				} else {
					alreadyCanonical = true;
				}
			}
		}
		if (stale) {
			AddDebugLogLineN(logKnownFiles,
				CFormat("SafeAddKFile: rename-during-hash swap, "
					"detaching stale '%s' for live '%s'") %
					stale->GetFilePath().JoinPaths(stale->GetFileName()) %
					toadd->GetFilePath().JoinPaths(toadd->GetFileName()));
			RemoveFile(stale);
			if (AddFile(toadd)) {
				Notify_SharedFilesShowFile(toadd);
			}
		} else if (alreadyCanonical) {
			// Same pointer, already the canonical shared entry, but its
			// path may have moved since it was first shared: a partfile
			// downloaded this session was keyed under the Temp dir and has
			// just been re-added from CPartFile::CompleteFileEnded() with
			// SetFilePath(Incoming). AddFile()'s insert no-ops here, so the
			// index still points at the stale Temp path and the dir-watcher
			// can't resolve a DELETE of the completed file. Re-key it.
			RefreshPathIndex(toadd);
			// A download just completed. Because the file was shared as a
			// partfile, its hash was already in the map, so AddFile()'s
			// insert (which is what normally schedules the media probe) no-op'd
			// above -- a media download would otherwise only get its FT_MEDIA_*
			// tags on the next startup rescan. Now that it is complete on disk
			// at its Incoming path, schedule the probe here. QueueProbe() only
			// enqueues (it never runs ffprobe inline), so this cannot stall the
			// completion. Completion mode bypasses BOTH gates -- the file is
			// still a CPartFile object at this point -- so
			// the authoritative local probe overwrites any metadata inherited
			// from the search result, which is only a during-download preview.
			MaybeScheduleMediaProbe(toadd, MediaProbeMode::Completion);
		}
	}

	if (!bOnlyAdd && theApp->IsConnectedED2K()) {
		// Publishing of files is not anymore handled here.
		// Instead, the timer does it by itself.
		m_lastPublishED2KFlag = true;
	}
}

void CSharedFileList::RefreshPathIndex(CKnownFile *file)
{
	if (!file) {
		return;
	}
	wxMutexLocker lock(list_mut);
	const wxString key = NormalizePathKey(file->GetFilePath().JoinPaths(file->GetFileName()).GetRaw());
	// Drop any stale keys pointing at this file (the pre-completion
	// Temp/<name> entry, or the "" placeholder from a not-yet-pathed
	// CPartFile) so the index maps only its current on-disk location.
	// O(shared files), but only runs on the rare re-add of an
	// already-shared file (download completion / re-share).
	bool rekeyed = false;
	for (std::unordered_map<wxString, CKnownFile *>::iterator it = m_pathIndex.begin();
		it != m_pathIndex.end();) {
		if (it->second == file && it->first != key) {
			it = m_pathIndex.erase(it);
			rekeyed = true;
		} else {
			++it;
		}
	}
	m_pathIndex[key] = file;
	if (rekeyed) {
		// The file moved without entering or leaving m_Files_map, so nothing
		// else bumps the generation for it: AddFile()'s insert no-ops on a
		// hash that is already shared. Anything caching a view of where files
		// live has to be told, or it keeps serving the old location -- the
		// directory grouping in GetSharedFilesByDirectory() would leave a
		// completed download filed under Temp until some unrelated add or
		// remove happened to invalidate it (issue #898). The pairwise walk it
		// replaced re-read GetFilePath() every time and so never had to be
		// told at all.
		m_listGeneration.fetch_add(1, std::memory_order_relaxed);
		AddDebugLogLineN(
			logKnownFiles, CFormat("Path index re-keyed to '%s' (file moved/completed)") % key);
	}
}

// removes first occurrence of 'toremove' in 'list'
void CSharedFileList::RemoveFile(CKnownFile *toremove)
{
	Notify_SharedFilesRemoveFile(toremove);
	wxMutexLocker lock(list_mut);
	if (m_Files_map.erase(toremove->GetFileHash()) > 0) {
		m_listGeneration.fetch_add(1, std::memory_order_relaxed);
		theStats::RemoveSharedFile(toremove->GetFileSize());
	}
	// Same path key we wrote into the index in AddFile(). erase() is a
	// no-op if the entry isn't present (e.g. the file was inserted
	// before m_pathIndex existed in an older save snapshot).
	//
	// The premise -- that the key we recompute here is the key AddFile wrote
	// -- holds only while GetFilePath() still matches what was indexed. When
	// it does not, this erase silently misses and leaves the real entry
	// behind, which then makes the file permanently unshareable because
	// NotifyPathAdded short-circuits on it. That is invisible without the
	// diagnostic below, which is how issue #1017 survived unnoticed.
	const wxString key =
		NormalizePathKey(toremove->GetFilePath().JoinPaths(toremove->GetFileName()).GetRaw());
	const size_t erasedFromIndex = m_pathIndex.erase(key);
	// `reloading` suppresses the check for the duration of a walk. The index is
	// cleared at the start of one and refilled as the walk proceeds, so a
	// removal that lands mid-walk -- CUploadDiskIOThread calls RemoveFile from
	// a worker thread -- legitimately finds nothing to erase. Without this the
	// hardening would cry wolf on every such removal, which is worse than not
	// having it (issue #1028).
	if (erasedFromIndex == 0 && !m_pathIndex.empty() && !reloading) {
		// Not fatal on its own -- the older-snapshot case above is legitimate
		// -- but it is the signature of a desynchronised index, so say so
		// rather than leaking an entry in silence.
		AddDebugLogLineC(logKnownFiles,
			CFormat("Path index: no entry under '%s' to erase for a file being removed from "
				"shares; the index may be out of step with the file's path") %
				key);
	}
	/* This file keywords must not be published to kad anymore */
	m_keywords->RemoveKeywords(toremove);
}

// Incremental rescan entry points used by CSharedDirWatcher.
//
// These exist so the watcher can apply a single fs-watcher event
// without firing the bulk Reload() path, which on a 100 k+ file
// shareset blocks the GUI for minutes per event. See issue #745.

void CSharedFileList::NotifyPathAdded(const wxString &fullPath, bool bulkScan)
{
	if (fullPath.IsEmpty()) {
		return;
	}

	// Already shared? CPartFile::CompleteFile() and SafeAddKFile() are
	// the canonical add paths for completed downloads — by the time
	// the watcher's CREATE event fires for a freshly-renamed file in
	// Incoming, the CKnownFile is usually already in m_Files_map and
	// the path index. Nothing to do in that case. Scoped lock so we
	// drop list_mut before doing any filesystem work.
	{
		wxMutexLocker existsCheck(list_mut);
		if (m_pathIndex.find(NormalizePathKey(fullPath)) != m_pathIndex.end()) {
			return;
		}
	}

	CPath full(fullPath);
	if (!full.IsOk()) {
		return;
	}
	const CPath directory = full.GetPath();
	const CPath fname = CPath(full.GetFullName());
	if (!directory.IsOk() || !fname.IsOk()) {
		return;
	}

	TaskList hashTasks;
	switch (AddPathToShares(directory, fname, hashTasks, /*notifyGuiOnKnownAdd=*/true)) {
	case kAddPathQueued:
		// Hand the new hashing task to the scheduler. The thread
		// will call SafeAddKFile() when it finishes, which is
		// what publishes the file to peers + the GUI.
		for (TaskList::iterator it = hashTasks.begin(); it != hashTasks.end(); ++it) {
			if (CThreadScheduler::AddTask(*it)) {
				++m_discoveredNewFiles;
			}
		}
		break;
	case kAddPathKnown:
		// A file already in known.met, moved or renamed into a shared
		// directory: nothing is hashed and nothing is probed, so without this
		// the file would silently become shared and be published to peers with
		// no info-level record at all (issue #968).
		//
		// Deliberately here and not inside AddPathToShares, which the bulk walk
		// also calls: there the already-known case is the overwhelming majority
		// of entries, and one line per file would bury everything else under
		// thousands of "nothing happened" lines on every rescan. The watcher
		// path is different in kind -- it is an event, it fires at
		// unpredictable times, its volume is bounded by real filesystem
		// activity rather than by tree size, and no summary line covers it.
		if (bulkScan) {
			// One line per file would be thousands during a tree walk; the
			// tick prints a single summary instead.
			++m_attachedKnownFiles;
		} else {
			AddLogLineN(CFormat(_("Now sharing file: %s")) % fullPath);
		}
		break;
	case kAddPathAlreadyShared:
	case kAddPathExcluded:
	case kAddPathSkipped:
		// AddPathToShares already wrote a debug log line; no
		// further action needed. Already-shared in particular must not
		// announce a share that did not happen.
		break;
	}
}

void CSharedFileList::NotifyPathRemoved(const wxString &fullPath)
{
	if (fullPath.IsEmpty()) {
		return;
	}

	// RemoveFile re-acquires list_mut itself, so we hold list_mut
	// only long enough to resolve the path → CKnownFile* lookup and
	// then drop it before calling RemoveFile.
	CKnownFile *file = NULL;
	{
		wxMutexLocker lock(list_mut);
		auto it = m_pathIndex.find(NormalizePathKey(fullPath));
		if (it == m_pathIndex.end()) {
			return;
		}
		file = it->second;
	}

	// Symmetric with "Now sharing file" above: a shared file disappearing
	// behind the user's back is at least as interesting as one appearing.
	AddLogLineN(CFormat(_("Stopped sharing removed file: %s")) % fullPath);
	AddDebugLogLineN(
		logKnownFiles, CFormat("Watcher: detaching deleted file '%s' from shares") % fullPath);
	RemoveFile(file);
}

void CSharedFileList::NotifyDirRemoved(const wxString &dirPath)
{
	if (dirPath.IsEmpty()) {
		return;
	}

	// Trailing separator so ".../Season 1" does not match sibling ".../Season 10".
	// NFC-fold to match the normalized keys stored in m_pathIndex (see NormalizePathKey).
	wxString prefix = NormalizePathKey(dirPath);
	const wxString sep(wxFileName::GetPathSeparator());
	if (!prefix.EndsWith(sep)) {
		prefix += sep;
	}

	// Collect under the lock, detach outside it: RemoveFile re-locks and erases
	// from m_pathIndex.
	std::vector<CKnownFile *> victims;
	{
		wxMutexLocker lock(list_mut);
		for (std::unordered_map<wxString, CKnownFile *>::const_iterator it = m_pathIndex.begin();
			it != m_pathIndex.end();
			++it) {
			if (it->first.StartsWith(prefix)) {
				victims.push_back(it->second);
			}
		}
	}

	if (victims.empty()) {
		return;
	}

	// One summary line, not one per file: a removed subtree can hold thousands
	// of files and the count is already in hand.
	//
	// The count is what this call actually detached, which on some backends is
	// only part of the subtree. macOS FSEvents delivers per-file DELETEs racing
	// the directory DELETE, so NotifyPathRemoved above already detached some
	// files individually (each with its own line) and only the remainder is
	// left for this sweep -- removing a 6-file directory was observed as four
	// per-file lines plus a summary saying two. Every file is still logged
	// exactly once and none is double-counted, so the accounting is right even
	// though the shape is not the single tidy line it is on a backend that
	// coalesces the subtree into one event. Reporting the directory's original
	// size instead would be a lie about what this call did, and suppressing the
	// per-file lines would mean losing them whenever the directory event never
	// arrives at all.
	AddLogLineN(CFormat(wxPLURAL("Stopped sharing %u file under removed directory: %s",
			    "Stopped sharing %u files under removed directory: %s",
			    static_cast<unsigned>(victims.size()))) %
		    static_cast<unsigned>(victims.size()) % dirPath);
	AddDebugLogLineN(logKnownFiles,
		CFormat("Watcher: detaching %zu shared file(s) under removed dir '%s'") % victims.size() %
			dirPath);
	for (std::vector<CKnownFile *>::iterator it = victims.begin(); it != victims.end(); ++it) {
		RemoveFile(*it);
	}
}

void CSharedFileList::NotifyPathModified(const wxString &fullPath)
{
	if (fullPath.IsEmpty()) {
		return;
	}

	// MODIFY events fire on metadata touches (utime, chmod, etc.) as
	// well as on content writes. Only a size/mtime delta warrants
	// re-hashing. Look up the file in the path index and compare its
	// known mtime/size against what's on disk.
	CKnownFile *file = NULL;
	{
		wxMutexLocker lock(list_mut);
		auto it = m_pathIndex.find(NormalizePathKey(fullPath));
		if (it == m_pathIndex.end()) {
			// Path appeared via MODIFY but wasn't already shared
			// — treat as add. List_mut is dropped at scope exit
			// before NotifyPathAdded re-acquires it.
			file = NULL;
		} else {
			file = it->second;
		}
	}
	if (file == NULL) {
		NotifyPathAdded(fullPath);
		return;
	}

	CPath full(fullPath);
	time_t fdiskDate = CPath::GetModificationTime(full);
	sint64 fdiskSize = full.GetFileSize();

	if (fdiskDate == (time_t)-1 || fdiskSize == wxInvalidOffset) {
		// File vanished or unreadable. Treat as removal.
		AddDebugLogLineN(logKnownFiles,
			CFormat("Watcher: file '%s' became unreadable on MODIFY, detaching") % fullPath);
		RemoveFile(file);
		return;
	}

	if (fdiskDate == file->GetLastChangeDatetime() && fdiskSize == (sint64)file->GetFileSize()) {
		// Same size, same mtime — content unchanged. Drop the event.
		return;
	}

	// Size or mtime moved. Content has changed and the existing
	// hashes are stale. Detach + re-add forces a fresh CHashingTask.
	AddDebugLogLineN(logKnownFiles,
		CFormat("Watcher: content changed on '%s' (size/mtime delta), re-hashing") % fullPath);
	RemoveFile(file);
	NotifyPathAdded(fullPath);
}

void CSharedFileList::Reload()
{
	Reload(nullptr);
}

bool CSharedFileList::Reload(ReloadYieldCb yieldCb)
{
	// Madcat - Disable reloading if reloading already in progress.
	// Kry - Fixed to let non-english language users use the 'Reload' button :P
	// deltaHF - removed the old ugly button and changed the code to use the new small one
	// Kry - bah, let's use a var.
	if (reloading) {
		// Already running. The walk in flight started before this caller's
		// roots or filters were in place, so it cannot be the fresh scan they
		// asked for -- leave a request standing so the next tick runs one.
		// Without this a caller that commits new shared roots and reads the
		// return as success persists them and never walks them.
		//
		// Surfaced to the caller as a non-abort, non-complete state: they
		// shouldn't react as if they cancelled, but haven't completed a
		// fresh scan either.
		m_reloadLatch.Request();
		return true;
	}

	// Take any outstanding RequestReload() with us: this walk is the one that
	// satisfies it, so a GUI caller can run it (with progress) right after
	// something requested one without Process() running a second, redundant
	// walk a tick later. Anything requested from here on belongs to the next
	// walk -- this one is already past the files it would be about -- and an
	// abort below hands this request back rather than swallowing it.
	const bool servingRequest = m_reloadLatch.BeginWalk();

	// Info, not debug: now that EC callers get an immediate reply instead of
	// blocking until the walk ends, the log is how they observe it starting.
	// The end-of-walk "Found %i known shared files" summary is already an
	// info line, so the two form a matched pair in release builds.
	AddLogLineN(_("Reloading shared files..."));
	reloading = true;
	Notify_SharedFilesRemoveAllItems();

	/* All Kad keywords must be removed.
	 *
	 * m_keywords has no internal locking; CSharedFileList::list_mut is
	 * the outer lock for both m_Files_map and m_keywords (every other
	 * AddFile / RemoveFile call takes it around m_keywords operations).
	 * Without the lock here we race CUploadDiskIOThread, which calls
	 * theApp->sharedfiles->RemoveFile(srcfile) from a worker thread when
	 * a previously-shared file disappears under it (e.g. user renaming
	 * a file in Incoming with shared-dir watching enabled, issue #685).
	 * The worker holds list_mut while it mutates m_keywords via
	 * RemoveKeywords; concurrent unlocked iteration over m_lstKeywords /
	 * m_keywordIndex here invalidates iterators / uses freed
	 * CPublishKeyword*.  Lock only around the keyword ops, NOT around
	 * FindSharedFiles -- that walks the filesystem and would block the
	 * worker pool for seconds at a time. */
	{
		wxMutexLocker lock(list_mut);
		m_keywords->RemoveAllKeywordReferences();
	}

	/* Public identifiers must be erased as they might be invalid now */
	{
		// Under list_mut: IsShared() builds m_sharedDirKeys through a const
		// method and takes the lock to do it, so this side has to take it too
		// or the lock buys nothing -- a reader would still be filling the set
		// while this clears it. The public-name map and its index are cleared
		// in the same scope so the pair cannot be seen half-emptied.
		wxMutexLocker lock(list_mut);
		m_PublicSharedDirNames.clear();
		m_publicNameByDirKey.clear();
		m_sharedDirKeys.clear();
		m_sharedDirKeysBuilt = false;
	}

	bool aborted = false;
	FindSharedFiles(yieldCb, aborted);

	/* And now the unreferenced keywords must be removed also */
	{
		wxMutexLocker lock(list_mut);
		m_keywords->PurgeUnreferencedKeywords();
	}

	Notify_SharedFilesShowFileList();

	// Re-sync the watcher's path set so dirs added or removed from
	// shareddir_list since the previous Reload are picked up.
	if (m_dirWatcher) {
		m_dirWatcher->Refresh();
	}

	// Tell KnownFileList that a full scan has now run -- this
	// gates the duplicate-list cap-prune in Save(), so the prune
	// never fires while the pin set is unpopulated (which would
	// drop records the scan was about to pin). Only on non-aborted
	// scans: a cancelled mid-scan leaves the pin set partial.
	// A cancelled walk satisfies nothing, so give the request back and let a
	// later tick run it properly.
	m_reloadLatch.EndWalk(servingRequest, aborted);

	if (!aborted && filelist) {
		filelist->MarkInitialShareScanComplete();
	}

	reloading = false;
	return !aborted;
}

const CKnownFile *CSharedFileList::GetFileByIndex(unsigned int index) const
{
	wxMutexLocker lock(list_mut);
	if (index >= m_Files_map.size()) {
		return NULL;
	}
	CKnownFileMap::const_iterator pos = m_Files_map.begin();
	std::advance(pos, index);
	return pos->second;
}

CKnownFile *CSharedFileList::GetFileByID(const CMD4Hash &filehash)
{
	wxMutexLocker lock(list_mut);
	CKnownFileMap::iterator it = m_Files_map.find(filehash);

	if (it != m_Files_map.end()) {
		return it->second;
	} else {
		return NULL;
	}
}

short CSharedFileList::GetFilePriorityByID(const CMD4Hash &filehash)
{
	CKnownFile *tocheck = GetFileByID(filehash);
	if (tocheck)
		return tocheck->GetUpPriority();
	else
		return -10; // file doesn't exist
}

void CSharedFileList::CopyFileList(std::vector<CKnownFile *> &out_list) const
{
	wxMutexLocker lock(list_mut);

	out_list.reserve(m_Files_map.size());
	for (CKnownFileMap::const_iterator it = m_Files_map.begin(); it != m_Files_map.end(); ++it) {
		out_list.push_back(it->second);
	}
}

void CSharedFileList::GetSharedFileNames(wxArrayString &out) const
{
	wxMutexLocker lock(list_mut);

	out.Alloc(m_Files_map.size());
	for (const auto &entry : m_Files_map) {
		out.Add(entry.second->GetFileName().GetPrintable());
	}
}

void CSharedFileList::UpdateItem(CKnownFile *toupdate)
{
	Notify_SharedFilesUpdateItem(toupdate);
}

void CSharedFileList::GetSharedFilesByDirectory(const wxString &directory, CKnownFilePtrList &list)
{
	wxMutexLocker lock(list_mut);

	// Answered from the grouping rather than by walking every shared file:
	// a browsing peer asks one directory at a time, and the walk made that
	// O(directories x files) IsSameDir() calls, each normalising both paths.
	// See m_dirGroups (issue #898).
	const uint64 generation = m_listGeneration.load(std::memory_order_relaxed);
	if (!m_dirGroupsBuilt || m_dirGroupsAt != generation) {
		m_dirGroups.clear();
		for (const auto &entry : m_Files_map) {
			CKnownFile *cur_file = entry.second;
			m_dirGroups[cur_file->GetFilePath().GetDirKey()].push_back(cur_file);
		}
		m_dirGroupsAt = generation;
		m_dirGroupsBuilt = true;
	}

	const std::map<wxString, CKnownFilePtrList>::const_iterator group =
		m_dirGroups.find(CPath(directory).GetDirKey());
	if (group != m_dirGroups.end()) {
		list.insert(list.end(), group->second.begin(), group->second.end());
	}
}

/* ---------------- Network ----------------- */

void CSharedFileList::ClearED2KPublishInfo()
{
	CKnownFile *cur_file;
	m_lastPublishED2KFlag = true;
	wxMutexLocker lock(list_mut);
	// Suppress per-row GUI updates while we walk every shared file.
	// SetPublishedED2K() notifies the SharedFilesCtrl which does an
	// O(N) FindItem per call; without this, a 100k-file shared list
	// makes every server disconnect freeze the main thread for
	// minutes. SetPublishedED2K() is also a no-op when the value
	// didn't change, so the genuinely-false→false majority is free.
	// See #302.
	Notify_SharedFilesBeginBulkUpdate();
	for (CKnownFileMap::iterator pos = m_Files_map.begin(); pos != m_Files_map.end(); ++pos) {
		cur_file = pos->second;
		cur_file->SetPublishedED2K(false);
	}
	Notify_SharedFilesEndBulkUpdate();
}

void CSharedFileList::ClearKadSourcePublishInfo()
{
	wxMutexLocker lock(list_mut);
	CKnownFile *cur_file;
	for (CKnownFileMap::iterator pos = m_Files_map.begin(); pos != m_Files_map.end(); ++pos) {
		cur_file = pos->second;
		cur_file->SetLastPublishTimeKadSrc(0, 0);
	}
}

void CSharedFileList::RepublishFile(CKnownFile *pFile)
{
	CServer *server = theApp->serverconnect->GetCurrentServer();
	if (server && (server->GetTCPFlags() & SRV_TCPFLG_COMPRESSION)) {
		m_lastPublishED2KFlag = true;
		pFile->SetPublishedED2K(false); // FIXME: this creates a wrong 'No' for the ed2k shared info
						// in the listview until the file is shared again.
	}
}

static uint8 GetRealPrio(uint8 in)
{
	switch (in) {
	case 4:
		return 0;
	case 0:
		return 1;
	case 1:
		return 2;
	case 2:
		return 3;
	case 3:
		return 4;
	}
	return 0;
}

static bool SortFunc(const CKnownFile *fileA, const CKnownFile *fileB)
{
	return GetRealPrio(fileA->GetUpPriority()) < GetRealPrio(fileB->GetUpPriority());
}

void CSharedFileList::SendListToServer()
{
	std::vector<CKnownFile *> SortedList;

	{
		wxMutexLocker lock(list_mut);

		if (m_Files_map.empty() || !theApp->IsConnectedED2K()) {
			return;
		}

		// Getting a sorted list of the non-published files.
		SortedList.reserve(m_Files_map.size());

		CKnownFileMap::iterator it = m_Files_map.begin();
		for (; it != m_Files_map.end(); ++it) {
			if (!it->second->GetPublishedED2K()) {
				SortedList.push_back(it->second);
			}
		}
	}

	std::sort(SortedList.begin(), SortedList.end(), SortFunc);

	// Limits for the server.

	CServer *server = theApp->serverconnect->GetCurrentServer();
	if (!server) {
		return;
	}

	uint32 limit = server->GetSoftFiles();
	if (limit == 0 || limit > 200) {
		limit = 200;
	}

	if ((uint32)SortedList.size() < limit) {
		limit = SortedList.size();
		if (limit == 0) {
			m_lastPublishED2KFlag = false;
			return;
		}
	}

	CMemFile files;

	// Files-sent count is patched in after the loop. We can't write the
	// final number up-front because the loop body filters out >4GB files
	// when the server doesn't advertise SRV_TCPFLG_LARGEFILES, and we
	// only know how many actually made it into the packet once the loop
	// has run. Pre-fix the header was hard-coded to `limit`, so the
	// packet header claimed N files but the body could carry N-K of them
	// for any K >4GB files in the prefix; legacy non-LF servers see a
	// short read against the count and may reject or partially process
	// the publish (#347).
	files.WriteUInt32(0);

	uint32 count = 0;
	// Add to packet
	std::vector<CKnownFile *>::iterator sorted_it = SortedList.begin();
	for (; (sorted_it != SortedList.end()) && (count < limit); ++sorted_it) {
		CKnownFile *file = *sorted_it;
		if (!file->IsLargeFile() || server->SupportsLargeFilesTCP()) {
			file->CreateOfferedFilePacket(&files, server, NULL);
			++count;
		}
		file->SetPublishedED2K(true);
	}

	// Nothing to publish to this server (e.g. every unpublished file in
	// our prefix is >4GB and the server doesn't advertise
	// SRV_TCPFLG_LARGEFILES). Sending an OP_OFFERFILES with count=0
	// would just be ~28 bytes of TCP overhead per ED2KREPUBLISHTIME
	// tick — the server gets no information from "0 offered" that it
	// didn't already have from us being silent.
	if (count == 0) {
		return;
	}

	// Patch the count to match what we actually wrote.
	files.Seek(0);
	files.WriteUInt32(count);

	CPacket *packet = new CPacket(files, OP_EDONKEYPROT, OP_OFFERFILES);
	// compress packet
	//   - this kind of data is highly compressible (N * (1 MD4 and at least 3 string meta data tags and 1
	//   integer meta data tag))
	//   - the min. amount of data needed for one published file is ~100 bytes
	//   - this function is called once when connecting to a server and when a file becomes shareable -
	//   so, it's called rarely.
	//   - if the compressed size is still >= the original size, we send the uncompressed packet
	// therefore we always try to compress the packet
	if (server->GetTCPFlags() & SRV_TCPFLG_COMPRESSION) {
		packet->PackPacket();
	}

	theStats::AddUpOverheadServer(packet->GetPacketSize());
	theApp->serverconnect->SendPacket(packet, true);
}

void CSharedFileList::Process()
{
	// Deferred reloads requested by callers on the core event loop (EC
	// handlers, the watcher's dropped-events fallback) run here rather than
	// inline in the caller. The `reloading` check means a request that
	// arrives mid-walk stays pending and runs on a later tick instead of
	// re-entering; Reload() would return early anyway, silently dropping it.
	if (m_reloadLatch.ShouldStartFromTick(reloading)) {
		Reload();
	}

	// Flushed after the drain above, so that when a reload runs on this tick
	// its count is printed immediately after its own "Found N known shared
	// files" summary rather than arriving a second later, detached from it.
	// Only when non-zero: a pass that discovered nothing says nothing.
	// Already-known files attached by a bulk subdirectory scan: one line for
	// the batch, mirroring the summary the removal side emits.
	if (m_attachedKnownFiles) {
		AddLogLineN(CFormat(wxPLURAL("Now sharing %u file found in a new shared directory",
				    "Now sharing %u files found in new shared directories",
				    m_attachedKnownFiles)) %
			    m_attachedKnownFiles);
		m_attachedKnownFiles = 0;
	}

	if (m_discoveredNewFiles) {
		AddLogLineN(CFormat(wxPLURAL("Discovered %u new shared file",
				    "Discovered %u new shared files",
				    m_discoveredNewFiles)) %
			    m_discoveredNewFiles);
		m_discoveredNewFiles = 0;
	}

	Publish();
	if (!m_lastPublishED2KFlag || (::GetTickCount64() - m_lastPublishED2K < ED2KREPUBLISHTIME)) {
		return;
	}
	SendListToServer();
	m_lastPublishED2K = ::GetTickCount64();
}

void CSharedFileList::Publish()
{
	// Variables to save cpu.
	unsigned int tNow = time(NULL);
	bool IsFirewalled = theApp->IsFirewalled();

	if (Kademlia::CKademlia::IsConnected() &&
		(!IsFirewalled || (IsFirewalled && theApp->clientlist->GetBuddyStatus() == Connected)) &&
		GetCount() && Kademlia::CKademlia::GetPublish()) {
		// We are connected to Kad. We are either open or have a buddy. And Kad is ready to start
		// publishing.

		if (Kademlia::CKademlia::GetTotalStoreKey() < KADEMLIATOTALSTOREKEY) {

			// list_mut serialises CPublishKeywordList access against
			// CUploadDiskIOThread's RemoveFile -> RemoveKeywords path,
			// which mutates pPubKw->references and ref counts from a
			// worker thread.  Without the lock the cursor advance and
			// the GetReferences() iteration below race with that path
			// (issue #685).  Kad's StartSearch / Go / GetClosestTo /
			// SendFindValue do not re-enter list_mut, so the lock can
			// be held across the Kad call.
			wxMutexLocker lock(list_mut);

			// We are not at the max simultaneous keyword publishes
			if (tNow >= m_keywords->GetNextPublishTime()) {

				// Enough time has passed since last keyword publish

				// Get the next keyword which has to be (re)-published
				CPublishKeyword *pPubKw = m_keywords->GetNextKeyword();
				if (pPubKw) {

					// We have the next keyword to check if it can be published

					// Debug check to make sure things are going well.
					wxASSERT(pPubKw->GetRefCount() != 0);

					if (tNow >= pPubKw->GetNextPublishTime()) {
						// This keyword can be published.
						Kademlia::CSearch *pSearch =
							Kademlia::CSearchManager::PrepareLookup(
								Kademlia::CSearch::STOREKEYWORD,
								false,
								pPubKw->GetKadID());
						if (pSearch) {
							// pSearch was created. Which means no search was
							// already being done with this HashID. This also
							// means that it was checked to see if network load
							// wasn't a factor.

							// This sets the filename into the search object so we
							// can show it in the gui.
							pSearch->SetFileName(pPubKw->GetKeyword());

							// Add all file IDs which relate to the current
							// keyword to be published
							const KnownFileArray &aFiles =
								pPubKw->GetReferences();
							uint32 count = 0;
							for (unsigned int f = 0; f < aFiles.size(); ++f) {

								// Only publish complete files as someone else
								// should have the full file to publish these
								// keywords. As a side effect, this may help
								// reduce people finding incomplete files in
								// the network.
								if (!aFiles[f]->IsPartFile()) {
									count++;
									pSearch->AddFileID(Kademlia::CUInt128(
										aFiles[f]
											->GetFileHash()
											.GetHash()));
									if (count > 150) {
										// We only publish up to 150
										// files per keyword publish
										// then rotate the list.
										pPubKw->RotateReferences(f);
										break;
									}
								}
							}

							if (count) {
								// Start our keyword publish
								pPubKw->SetNextPublishTime(
									tNow + (KADEMLIAREPUBLISHTIMEK));
								pPubKw->IncPublishedCount();
								Kademlia::CSearchManager::StartSearch(
									pSearch);
							} else {
								// There were no valid files to publish with
								// this keyword.
								delete pSearch;
							}
						}
					}
				}
				m_keywords->SetNextPublishTime(KADEMLIAPUBLISHTIME + tNow);
			}
		}

		if (Kademlia::CKademlia::GetTotalStoreSrc() < KADEMLIATOTALSTORESRC) {
			if (tNow >= m_lastPublishKadSrc) {
				if (m_currFileSrc > GetCount()) {
					m_currFileSrc = 0;
				}
				CKnownFile *pCurKnownFile =
					const_cast<CKnownFile *>(GetFileByIndex(m_currFileSrc));
				if (pCurKnownFile) {
					if (pCurKnownFile->PublishSrc()) {
						Kademlia::CUInt128 kadFileID;
						kadFileID.SetValueBE(pCurKnownFile->GetFileHash().GetHash());
						if (Kademlia::CSearchManager::PrepareLookup(
							    Kademlia::CSearch::STOREFILE, true, kadFileID) ==
							NULL) {
							pCurKnownFile->SetLastPublishTimeKadSrc(0, 0);
						}
					}
				}
				m_currFileSrc++;

				// even if we did not publish a source, reset the timer so that this list is
				// processed only every KADEMLIAPUBLISHTIME seconds.
				m_lastPublishKadSrc = KADEMLIAPUBLISHTIME + tNow;
			}
		}

		if (Kademlia::CKademlia::GetTotalStoreNotes() < KADEMLIATOTALSTORENOTES) {
			if (tNow >= m_lastPublishKadNotes) {
				if (m_currFileNotes > GetCount()) {
					m_currFileNotes = 0;
				}
				CKnownFile *pCurKnownFile =
					const_cast<CKnownFile *>(GetFileByIndex(m_currFileNotes));
				if (pCurKnownFile) {
					if (pCurKnownFile->PublishNotes()) {
						Kademlia::CUInt128 kadFileID;
						kadFileID.SetValueBE(pCurKnownFile->GetFileHash().GetHash());
						if (Kademlia::CSearchManager::PrepareLookup(
							    Kademlia::CSearch::STORENOTES, true, kadFileID) ==
							NULL)
							pCurKnownFile->SetLastPublishTimeKadNotes(0);
					}
				}
				m_currFileNotes++;

				// even if we did not publish a source, reset the timer so that this list is
				// processed only every KADEMLIAPUBLISHTIME seconds.
				m_lastPublishKadNotes = KADEMLIAPUBLISHTIME + tNow;
			}
		}
	}
}

void CSharedFileList::AddKeywords(CKnownFile *pFile)
{
	m_keywords->AddKeywords(pFile);
}

void CSharedFileList::RemoveKeywords(CKnownFile *pFile)
{
	m_keywords->RemoveKeywords(pFile);
}

void CSharedFileList::VerifyLocalData(const CKnownFile *file) const
{
	if (file)
		file->VerifyLocalData();
}

bool CSharedFileList::RenameFile(CKnownFile *file, const CPath &newName)
{
	if (file->IsPartFile()) {
		CPartFile *pfile = dynamic_cast<CPartFile *>(file);

		if (file->GetStatus() != PS_COMPLETING) {
			pfile->SetFileName(newName);
			pfile->SavePartFile();

			Notify_SharedFilesUpdateItem(file);
			Notify_DownloadCtrlUpdateItem(file);

			return true;
		}
	} else {
		CPath oldPath = file->GetFilePath().JoinPaths(file->GetFileName());
		CPath newPath = file->GetFilePath().JoinPaths(newName);

		if (CPath::RenameFile(oldPath, newPath)) {
			// Must create a copy of the word list because:
			// 1) it will be reset on SetFileName()
			// 2) we will want to edit it
			Kademlia::WordList oldwords = file->GetKadKeywords();
			file->SetFileName(newName);
			theApp->knownfiles->Save();
			UpdateItem(file);
			RepublishFile(file);

			const Kademlia::WordList &newwords = file->GetKadKeywords();
			Kademlia::WordList::iterator it_old;
			Kademlia::WordList::const_iterator it_new;
			// compare keywords in old and new names
			for (it_new = newwords.begin(); it_new != newwords.end(); ++it_new) {
				for (it_old = oldwords.begin(); it_old != oldwords.end(); ++it_old) {
					if (*it_old == *it_new) {
						break;
					}
				}
				if (it_old != oldwords.end()) {
					// Remove keyword from old name which also exist in new name
					oldwords.erase(it_old);
				} else {
					// This is a new keyword not present in the old name
					m_keywords->AddKeyword(*it_new, file);
				}
			}
			// Remove all remaining old keywords not present in the new name
			for (it_old = oldwords.begin(); it_old != oldwords.end(); ++it_old) {
				m_keywords->RemoveKeyword(*it_old, file);
			}

			Notify_DownloadCtrlUpdateItem(file);
			Notify_SharedFilesUpdateItem(file);

			return true;
		}
	}

	return false;
}

const CPath *CSharedFileList::GetDirForPublicSharedDirName(const wxString &strSharedDir) const
{
	StringPathMap::const_iterator it = m_PublicSharedDirNames.find(strSharedDir);

	if (it != m_PublicSharedDirNames.end()) {
		return &(it->second);
	} else {
		return NULL;
	}
}

wxString CSharedFileList::GetPublicSharedDirName(const CPath &dir)
{
	// safety check: is the directory supposed to be shared after all?
	if (!IsShared(dir)) {
		wxFAIL;
		return "";
	}
	// check if the public name for the directory is cached in our Map.
	// Keyed rather than walked: this runs once per shared directory while
	// answering a browse, and comparing every entry with IsSameDir() made it
	// O(directories^2) (issue #898).
	//
	// Under list_mut, like the write further down and like IsShared(): a
	// mutex only excludes participants who take it, so a reader outside it
	// would make the locking on the other side worth nothing. Held for the
	// lookup alone -- not across the IsShared() calls above and below, which
	// take the same non-recursive mutex themselves.
	{
		wxMutexLocker lock(list_mut);
		const std::map<wxString, wxString>::const_iterator cached =
			m_publicNameByDirKey.find(dir.GetDirKey());
		if (cached != m_publicNameByDirKey.end()) {
			// public name for directory was determined earlier
			return cached->second;
		}
	}

	// we store the path separator (forward or back slash) for quick access
	wxChar cPathSeparator = wxFileName::GetPathSeparator();

	// determine and cache the public name for "dir" ...
	// We need to use the 'raw' filename, so the receiving client can recognize it.
	wxString strDirectoryTmp = dir.GetRaw();
	if (strDirectoryTmp.EndsWith(&cPathSeparator)) {
		strDirectoryTmp.RemoveLast();
	}

	wxString strPublicName;
	int iPos;
	// check all the subdirectories in the path for being shared
	// the public name will consist of these concatenated
	while ((iPos = strDirectoryTmp.Find(cPathSeparator, true)) != wxNOT_FOUND) {
		strPublicName = strDirectoryTmp.Right(strDirectoryTmp.Length() - iPos) + strPublicName;
		strDirectoryTmp.Truncate(iPos);
		if (!IsShared(CPath(strDirectoryTmp)))
			break;
	}
	if (!strPublicName.IsEmpty()) {
		// remove first path separator ???
		wxASSERT(strPublicName.GetChar(0) == cPathSeparator);
		strPublicName = strPublicName.Right(strPublicName.Length() - 1);
	} else {
		// must be a rootdirectory on Windows
		wxASSERT(strDirectoryTmp.Length() == 2);
		strPublicName = strDirectoryTmp;
	}
	// we have the name, make sure it is unique by appending an index if
	// necessary. Under list_mut for the same reason as the lookup above, and
	// covering both maps together so the pair is never left half-written --
	// which is what the clear in Reload() is holding the lock against. Safe
	// to take here: every IsShared() call, which takes the same mutex, is
	// behind us.
	wxMutexLocker lock(list_mut);
	if (m_PublicSharedDirNames.find(strPublicName) != m_PublicSharedDirNames.end()) {
		wxString strUniquePublicName;
		for (iPos = 2;; ++iPos) {
			strUniquePublicName = CFormat("%s_%i") % strPublicName % iPos;

			if (m_PublicSharedDirNames.find(strUniquePublicName) ==
				m_PublicSharedDirNames.end()) {
				AddDebugLogLineN(logClient,
					CFormat("Using public name '%s' for directory '%s'") %
						strUniquePublicName % dir.GetPrintable());
				m_publicNameByDirKey[dir.GetDirKey()] = strUniquePublicName;
				m_PublicSharedDirNames.insert(
					std::pair<wxString, CPath>(strUniquePublicName, dir));
				return strUniquePublicName;
			}
			// This is from eMule and it checks if there are more than 200 shared folders with the
			// same public name. The condition can be true if many shared subfolders with the same
			// name exist in folders that are not shared. So they get the names of each shared
			// subfolders concatenated. But those might all be the same! It's here for safety
			// reasons so we should not run out of memory.
			else if (iPos > 200) // Only 200 identical names are indexed.
			{
				wxASSERT(false);
				return "";
			}
		}
	} else {
		AddDebugLogLineN(logClient,
			CFormat("Using public name '%s' for directory '%s'") % strPublicName %
				dir.GetPrintable());
		m_publicNameByDirKey[dir.GetDirKey()] = strPublicName;
		m_PublicSharedDirNames.insert(std::pair<wxString, CPath>(strPublicName, dir));
		return strPublicName;
	}
}

bool CSharedFileList::IsShared(const CPath &path) const
{
	if (path.IsDir(CPath::exists)) {
		// Under list_mut like every other cache on this class. The lazily
		// built set below is written through a const method, so without it a
		// caller on another thread would be writing a std::set while Reload()
		// cleared it. Today both callers sit in GetPublicSharedDirName() on
		// the main thread and Reload() runs there too, but nothing states
		// that, and this class carries a mutex precisely because the
		// assumption is not general. Neither call site holds the lock, so
		// there is nothing to deadlock against.
		wxMutexLocker lock(list_mut);

		// Both lists below were walked with IsSameDir(), which normalises
		// both paths, and GetPublicSharedDirName() calls this once per shared
		// directory while answering a browse -- O(directories^2), and the
		// other half of the 53 s freeze in issue #898. Keyed instead, built
		// once and dropped in Reload() where the set can change.
		if (!m_sharedDirKeysBuilt) {
			const unsigned folderCount = theApp->glob_prefs->shareddir_list.size();
			for (unsigned i = 0; i < folderCount; ++i) {
				m_sharedDirKeys.insert(theApp->glob_prefs->shareddir_list[i].GetDirKey());
			}
			// category 0 is incoming
			for (unsigned i = 0; i < theApp->glob_prefs->GetCatCount(); ++i) {
				m_sharedDirKeys.insert(theApp->glob_prefs->GetCategory(i)->path.GetDirKey());
			}
			m_sharedDirKeysBuilt = true;
		}

		if (m_sharedDirKeys.count(path.GetDirKey()) != 0) {
			return true;
		}
	}

	return false;
}

void CSharedFileList::CheckAICHHashes(const std::list<CAICHHash> &hashes)
{
	// Index the master-hash list up front: the inner check is otherwise a
	// linear std::find over `hashes` for every shared file, making the whole
	// loop O(N*M). On sharesets of 100 k+ files (issue #745) that walk holds
	// `list_mut` long enough to freeze the GUI for minutes. An unordered_set
	// keyed on CAICHHash (std::hash specialisation in SHAHashSet.h) makes
	// each lookup O(1) average and drops the total to O(N + M).
	const std::unordered_set<CAICHHash> hashIndex(hashes.begin(), hashes.end());

	wxMutexLocker locker(list_mut);

	// Now we check that all files which are in the sharedfilelist have a
	// corresponding hash in our list. Those who don't are queued for hashing.
	CKnownFileMap::iterator it = m_Files_map.begin();
	for (; it != m_Files_map.end(); ++it) {
		const CKnownFile *file = it->second;

		if (file->IsPartFile() == false) {
			CAICHHashSet *hashset = file->GetAICHHashset();

			if (hashset->GetStatus() == AICH_HASHSETCOMPLETE) {
				if (hashIndex.count(hashset->GetMasterHash()) > 0) {
					continue;
				}
			}

			hashset->SetStatus(AICH_ERROR);

			CThreadScheduler::AddTask(new CHashingTask(file));
		}
	}
}

// File_checked_for_headers
