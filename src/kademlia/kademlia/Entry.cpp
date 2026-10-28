//
// This file is part of the aMule Project.
//
// Copyright (c) 2008-2011 Dévai Tamás ( gonosztopi@amule.org )
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2003-2011 Barry Dunne (http://www.emule-project.net)
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

// Note To Mods //
/*
Please do not change anything here and release it..
There is going to be a new forum created just for the Kademlia side of the client..
If you feel there is an error or a way to improve something, please
post it in the forum first and let us look at it.. If it is a real improvement,
it will be added to the official client.. Changing something without knowing
what all it does can cause great harm to the network if released in mass form..
Any mod that changes anything within the Kademlia side will not be allowed to advertise
there client on the eMule forum..
*/

#include "Entry.h"
#include <algorithm>
#include <stdexcept>
#include <common/Macros.h>
#include <tags/FileTags.h>
#include <protocol/kad/Constants.h>
#include "Indexed.h"
#include "../../SafeFile.h"
#include "../../Preferences.h"
#include "../../GetTickCount.h"
#include "../../Logger.h"
#include "../../NetworkFunctions.h"

using namespace Kademlia;

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////// CEntry
CEntry::~CEntry()
{
	deleteTagPtrListEntries(&m_taglist);
}

CEntry *CEntry::Copy() const
{
	CEntry *entry = new CEntry();
	for (FileNameList::const_iterator it = m_filenames.begin(); it != m_filenames.end(); ++it) {
		entry->m_filenames.push_back(*it);
	}
	entry->m_uIP = m_uIP;
	entry->m_uKeyID = m_uKeyID;
	entry->m_tLifeTime = m_tLifeTime;
	entry->m_uSize = m_uSize;
	entry->m_bSource = m_bSource;
	entry->m_extraTagBytes = m_extraTagBytes;
	entry->m_uSourceID = m_uSourceID;
	entry->m_uTCPport = m_uTCPport;
	entry->m_uUDPport = m_uUDPport;
	for (TagPtrList::const_iterator it = m_taglist.begin(); it != m_taglist.end(); ++it) {
		entry->m_taglist.push_back((*it)->CloneTag());
	}
	return entry;
}

void CEntry::AddTag(CTag *tag, uint32_t dbgSourceIP)
{
	// Filter tags that exist for sending query results only and should never be stored, let
	// alone sent within the taglist. TAG_PUBLISHINFO is one we build when answering a keyword
	// search, out of our own count of distinct publishers and how much we trust them; stored, a
	// peer-supplied one travels back out of CKeyEntry::WriteTagListWithPublishInfo() alongside
	// our genuine value.
	//
	// TAG_KADAICHHASHRESULT is the same shape of tag and belongs in the same branch.
	// Deliberately ungated: the guard's value is that it is one unconditional branch covering
	// every caller, and filtering a tag this build never emits costs nothing. It also covers
	// the three storing paths a handler-side filter does not -- source and notes publishing,
	// and CIndexed::ReadFile(), which rebuilds every entry from key_index.dat with its raw
	// taglist, so a node poisoned before this fix would reload the tag on restart and keep
	// serving it.
	//
	// TAG_KADAICHHASHPUB is not here: that one is consumed into a member rather than filtered,
	// which is the split 0.70b, 0.72a, eMuleAI and emule-qt all use.
	if (!tag->GetName().Cmp(TAG_PUBLISHINFO) || !tag->GetName().Cmp(TAG_KADAICHHASHRESULT)) {
		AddDebugLogLineN(logKadEntryTracking,
			CFormat("Filtered result-only tag on storing, source %s") %
				(dbgSourceIP ? KadIPToString(dbgSourceIP) : wxString(wxT("local"))));
		delete tag;
		return;
	}
	m_taglist.push_back(tag);
}

bool CEntry::GetIntTagValue(const wxString &tagname, uint64_t &value, bool includeVirtualTags) const
{
	for (TagPtrList::const_iterator it = m_taglist.begin(); it != m_taglist.end(); ++it) {
		if ((*it)->IsInt() && ((*it)->GetName() == tagname)) {
			value = (*it)->GetInt();
			return true;
		}
	}

	if (includeVirtualTags) {
		// SizeTag is not stored anymore, but queried in some places
		if (tagname == TAG_FILESIZE) {
			value = m_uSize;
			return true;
		}
	}
	value = 0;
	return false;
}

wxString CEntry::GetStrTagValue(const wxString &tagname) const
{
	for (TagPtrList::const_iterator it = m_taglist.begin(); it != m_taglist.end(); ++it) {
		if (((*it)->GetName() == tagname) && (*it)->IsStr()) {
			return (*it)->GetStr();
		}
	}
	return "";
}

void CEntry::SetFileName(const wxString &name)
{
	// Empty filenames are protocol garbage: a peer publishing a Kad note or keyword with an
	// empty FT_FILENAME tag should never reach m_filenames. Without this guard the empty entry
	// takes the popularity slot, GetCommonFileName returns "" and trips its own invariant on
	// the next call (issue #674).
	if (name.IsEmpty()) {
		return;
	}
	if (!m_filenames.empty()) {
		wxFAIL;
		m_filenames.clear();
	}
	sFileNameEntry sFN = { name, 1 };
	m_filenames.push_front(sFN);
}

wxString CEntry::GetCommonFileName() const
{
	// Return the filename most publishers agreed on, by popularity index. The index is not the
	// actual count of publishers, just a relative number for comparing entries, so the choice
	// is approximate.
	//
	// The running max is seeded from the first entry rather than 0, so an all-zero m_filenames
	// still yields a non-empty result. Our own code never produces a popularity-0 entry, but
	// two paths we do not control can: a remote publisher sending one over the wire (ReadUInt32
	// does not validate), and on-disk data from a node that ran an older build doing popularity
	// decay. A "highest > 0" loop would then leave the result iterator at end() and return an
	// empty string, silently dropping TAG_FILENAME from search responses, making
	// SearchTermsMatch false for every term and tripping the reject path in
	// CIndexed::AddKeyword.
	if (m_filenames.empty()) {
		return wxString("");
	}
	FileNameList::const_iterator result = m_filenames.begin();
	uint32_t highestPopularityIndex = result->m_popularityIndex;
	for (FileNameList::const_iterator it = std::next(result); it != m_filenames.end(); ++it) {
		if (it->m_popularityIndex > highestPopularityIndex) {
			highestPopularityIndex = it->m_popularityIndex;
			result = it;
		}
	}
	wxASSERT(!result->m_filename.IsEmpty());
	return result->m_filename;
}

void CEntry::WriteTagListInc(CFileDataIO *data, uint32_t increaseTagNumber)
{
	wxCHECK_RET(data != NULL, "data must not be NULL");

	const uint32_t wanted =
		GetTagCount() + increaseTagNumber; // will include name and size tag in the count if needed

	// The count goes on the wire as one byte, so it cannot describe more than 255 tags. Writing
	// the truncated value and then all of them anyway is the worst of the options: the reader
	// takes the wrapped count, stops early, and parses the remaining tags as whatever field it
	// expected next, losing the rest of the packet. Clamp instead, so an over-full entry sends a
	// short answer that still parses. The caller writes increaseTagNumber tags of its own after
	// this returns, so they come out of the same budget.
	const uint32_t count = wanted > 0xFF ? 0xFF : wanted;
	if (wanted != count) {
		AddDebugLogLineN(logKadEntryTracking,
			CFormat("Kad entry has %u tags, more than the %u a search answer can "
				"describe; dropping the surplus") %
				wanted % count);
	}
	data->WriteUInt8((uint8_t)count);

	// What this function may write, once the caller's own tags are reserved.
	uint32_t budget = count > increaseTagNumber ? count - increaseTagNumber : 0;

	if (!GetCommonFileName().IsEmpty() && budget > 0) {
		data->WriteTag(CTagString(TAG_FILENAME, GetCommonFileName()));
		budget--;
	}
	if (m_uSize != 0 && budget > 0) {
		data->WriteTag(CTagVarInt(TAG_FILESIZE, m_uSize));
		budget--;
	}

	for (TagPtrList::const_iterator it = m_taglist.begin(); it != m_taglist.end() && budget > 0; ++it) {
		data->WriteTag(**it);
		budget--;
	}
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////// CKeyEntry
CKeyEntry::CKeyEntry(std::shared_ptr<PublishTracking> tracking)
: m_publishTracking(std::move(tracking))
{
	// Standalone entries (including codec tests) share the historical default.
	// An index supplies its own table, so a loading worker cannot touch live trust.
	if (!m_publishTracking) {
		static auto defaults = std::make_shared<PublishTracking>();
		m_publishTracking = defaults;
	}
	m_publishingIPs = NULL;
	m_trustValue = 0;
	m_lastTrustValueCalc = 0;
}

CKeyEntry::~CKeyEntry()
{
	if (m_publishingIPs != NULL) {
		for (PublishingIPList::const_iterator it = m_publishingIPs->begin();
			it != m_publishingIPs->end();
			++it) {
			AdjustGlobalPublishTracking(it->m_ip, false, "instance delete");
		}
		delete m_publishingIPs;
		m_publishingIPs = NULL;
	}
}

// Matching cost in units of about 0.1 ns, calibrated on the slowest C library measured (macOS):
// about 0.3 ns per character scanned or compared, 2 ns per character lowercased. The fixed parts
// stand for the call overhead, which dominates when names and words are short.
static const uint64_t NODE_COST = 64;
static const uint64_t LOOKUP_COST = 128;
static const uint64_t NAME_COST = 64;
static const uint64_t LOWER_CHAR_COST = 20;
// Per character scanned or compared.
static const uint64_t CHAR_COST = 3;
// Per position where the word's first character occurs: a compare call, plus the compare.
static const uint64_t CANDIDATE_COST = 48;
// Per tag a tag condition may visit while looking up its tag.
static const uint64_t TAG_COST = 48;
// Per character a case-insensitive compare may examine.
static const uint64_t NOCASE_CHAR_COST = 50;

struct CKeyEntry::MatchState
{
	uint64_t &budget;
	const CKeyEntry &entry;
	wxString nameLower;
	bool haveName = false;
	wxString extension;
	bool haveExtension = false;
	bool hasExtension = false;
	bool exhausted = false;

	MatchState(uint64_t &b, const CKeyEntry &e)
	: budget(b)
	, entry(e)
	{
	}

	bool Spend(uint64_t cost)
	{
		if (exhausted || cost > budget) {
			budget = 0;
			exhausted = true;
			return false;
		}
		budget -= cost;
		return true;
	}

	// Built on first use, then shared by every leaf of the tree. Null once the budget is spent.
	const wxString *NameLower()
	{
		if (!haveName) {
			nameLower = entry.GetCommonFileNameLowerCase();
			haveName = true;
			if (!Spend(NAME_COST + LOWER_CHAR_COST * nameLower.length())) {
				return nullptr;
			}
		}
		return exhausted ? nullptr : &nameLower;
	}

	// The extension of the lowercased name, built once per entry. Null when the name has none
	// or the budget is spent.
	const wxString *Extension()
	{
		if (!haveExtension) {
			const wxString *name = NameLower();
			if (name == nullptr) {
				return nullptr;
			}
			haveExtension = true;
			if (!Spend(LOOKUP_COST + CHAR_COST * name->length())) {
				return nullptr;
			}
			const int dot = name->Find('.', true);
			hasExtension = dot != wxNOT_FOUND;
			if (hasExtension) {
				extension = name->Mid(dot + 1);
			}
		}
		return exhausted || !hasExtension ? nullptr : &extension;
	}

	bool EqualNoCase(const wxString &a, const wxString &b)
	{
		return Spend(LOOKUP_COST + NOCASE_CHAR_COST * std::min(a.length(), b.length())) &&
		       a.CmpNoCase(b) == 0;
	}

	// A substring search pays for each position where the word's first character occurs, not
	// for every position: a name of "aaaa..." makes every position a compare, a name without
	// the character costs one scan. Returns false when not found or out of budget.
	bool Contains(const wxString &name, const wxString &word)
	{
		const size_t nameLen = name.length();
		const size_t wordLen = word.length();
		if (!Spend(LOOKUP_COST + CHAR_COST * nameLen)) {
			return false;
		}
		if (wordLen == 0) {
			return true;
		}
		const wxUniChar first = word[0];
		for (size_t pos = name.find(first); pos != wxString::npos && pos + wordLen <= nameLen;
			pos = name.find(first, pos + 1)) {
			if (!Spend(CANDIDATE_COST + CHAR_COST * wordLen)) {
				return false;
			}
			if (name.compare(pos, wordLen, word) == 0) {
				return true;
			}
		}
		return false;
	}
};

bool CKeyEntry::SearchTermsMatch(const SSearchTerm *searchTerm, uint64_t &budget) const
{
	MatchState state(budget, *this);
	// A NOT whose right side ran out of budget would otherwise match.
	return MatchTerm(searchTerm, state) && !state.exhausted;
}

bool CKeyEntry::MatchTerm(const SSearchTerm *searchTerm, MatchState &state) const
{
	if (!state.Spend(NODE_COST)) {
		return false;
	}

	if (searchTerm->type == SSearchTerm::AND) {
		return MatchTerm(searchTerm->left, state) && MatchTerm(searchTerm->right, state);
	}

	if (searchTerm->type == SSearchTerm::OR) {
		return MatchTerm(searchTerm->left, state) || MatchTerm(searchTerm->right, state);
	}

	if (searchTerm->type == SSearchTerm::NOT) {
		return MatchTerm(searchTerm->left, state) && !MatchTerm(searchTerm->right, state);
	}

	// word which is to be searched in the file name (and in additional meta data as done by some ed2k
	// servers???)
	if (searchTerm->type == SSearchTerm::String) {
		int strSearchTerms = searchTerm->astr->GetCount();
		if (strSearchTerms == 0) {
			return false;
		}
		// With more than one search string (e.g. "aaa bbb ccc") the whole thing is handled
		// as "aaa AND bbb AND ccc": every string has to be found in the tokenized file
		// name.
		const wxString *nameLower = state.NameLower();
		if (nameLower == nullptr) {
			return false;
		}
		for (int i = 0; i < strSearchTerms; i++) {
			// this will not give the same results as when tokenizing the filename string, but it
			// is 20 times faster.
			if (!state.Contains(*nameLower, (*(searchTerm->astr))[i])) {
				return false;
			}
		}
		return true;
	}

	// The remaining conditions look up their tag in the entry's tag list, which a keyword
	// publish can fill with up to 253 tags.
	if (!state.Spend(TAG_COST * m_taglist.size())) {
		return false;
	}

	if (searchTerm->type == SSearchTerm::MetaTag) {
		if (searchTerm->tag->GetType() == 2) { // meta tags with string values
			if (searchTerm->tag->GetName() == TAG_FILEFORMAT) {
				// 21-Sep-2006 []: Special handling for TAG_FILEFORMAT which is already part
				// of the filename and thus does not need to get published nor stored
				// explicitly,
				const wxString *extension = state.Extension();
				return extension != nullptr &&
				       state.EqualNoCase(*extension, searchTerm->tag->GetStr());
			} else {
				for (const CTag *tag : m_taglist) {
					if (tag->IsStr() && searchTerm->tag->GetName() == tag->GetName()) {
						return state.EqualNoCase(
							tag->GetStr(), searchTerm->tag->GetStr());
					}
				}
			}
		}
	} else if (searchTerm->type == SSearchTerm::OpGreaterEqual) {
		if (searchTerm->tag->IsInt()) { // meta tags with integer values
			uint64_t value;
			if (GetIntTagValue(searchTerm->tag->GetName(), value, true)) {
				return value >= searchTerm->tag->GetInt();
			}
		} else if (searchTerm->tag->IsFloat()) { // meta tags with float values
			for (TagPtrList::const_iterator it = m_taglist.begin(); it != m_taglist.end(); ++it) {
				if ((*it)->IsFloat() && searchTerm->tag->GetName() == (*it)->GetName()) {
					return (*it)->GetFloat() >= searchTerm->tag->GetFloat();
				}
			}
		}
	} else if (searchTerm->type == SSearchTerm::OpLessEqual) {
		if (searchTerm->tag->IsInt()) { // meta tags with integer values
			uint64_t value;
			if (GetIntTagValue(searchTerm->tag->GetName(), value, true)) {
				return value <= searchTerm->tag->GetInt();
			}
		} else if (searchTerm->tag->IsFloat()) { // meta tags with float values
			for (TagPtrList::const_iterator it = m_taglist.begin(); it != m_taglist.end(); ++it) {
				if ((*it)->IsFloat() && searchTerm->tag->GetName() == (*it)->GetName()) {
					return (*it)->GetFloat() <= searchTerm->tag->GetFloat();
				}
			}
		}
	} else if (searchTerm->type == SSearchTerm::OpGreater) {
		if (searchTerm->tag->IsInt()) { // meta tags with integer values
			uint64_t value;
			if (GetIntTagValue(searchTerm->tag->GetName(), value, true)) {
				return value > searchTerm->tag->GetInt();
			}
		} else if (searchTerm->tag->IsFloat()) { // meta tags with float values
			for (TagPtrList::const_iterator it = m_taglist.begin(); it != m_taglist.end(); ++it) {
				if ((*it)->IsFloat() && searchTerm->tag->GetName() == (*it)->GetName()) {
					return (*it)->GetFloat() > searchTerm->tag->GetFloat();
				}
			}
		}
	} else if (searchTerm->type == SSearchTerm::OpLess) {
		if (searchTerm->tag->IsInt()) { // meta tags with integer values
			uint64_t value;
			if (GetIntTagValue(searchTerm->tag->GetName(), value, true)) {
				return value < searchTerm->tag->GetInt();
			}
		} else if (searchTerm->tag->IsFloat()) { // meta tags with float values
			for (TagPtrList::const_iterator it = m_taglist.begin(); it != m_taglist.end(); ++it) {
				if ((*it)->IsFloat() && searchTerm->tag->GetName() == (*it)->GetName()) {
					return (*it)->GetFloat() < searchTerm->tag->GetFloat();
				}
			}
		}
	} else if (searchTerm->type == SSearchTerm::OpEqual) {
		if (searchTerm->tag->IsInt()) { // meta tags with integer values
			uint64_t value;
			if (GetIntTagValue(searchTerm->tag->GetName(), value, true)) {
				return value == searchTerm->tag->GetInt();
			}
		} else if (searchTerm->tag->IsFloat()) { // meta tags with float values
			for (TagPtrList::const_iterator it = m_taglist.begin(); it != m_taglist.end(); ++it) {
				if ((*it)->IsFloat() && searchTerm->tag->GetName() == (*it)->GetName()) {
					return (*it)->GetFloat() == searchTerm->tag->GetFloat();
				}
			}
		}
	} else if (searchTerm->type == SSearchTerm::OpNotEqual) {
		if (searchTerm->tag->IsInt()) { // meta tags with integer values
			uint64_t value;
			if (GetIntTagValue(searchTerm->tag->GetName(), value, true)) {
				return value != searchTerm->tag->GetInt();
			}
		} else if (searchTerm->tag->IsFloat()) { // meta tags with float values
			for (TagPtrList::const_iterator it = m_taglist.begin(); it != m_taglist.end(); ++it) {
				if ((*it)->IsFloat() && searchTerm->tag->GetName() == (*it)->GetName()) {
					return (*it)->GetFloat() != searchTerm->tag->GetFloat();
				}
			}
		}
	}

	return false;
}

void CKeyEntry::AdjustGlobalPublishTracking(uint32_t ip, bool increase, const wxString &DEBUG_ONLY(dbgReason))
{
	uint32_t count = 0;
	bool found = false;
	GlobalPublishIPMap::const_iterator it =
		m_publishTracking->find(ip & 0xFFFFFF00 /* /24 netmask, take care of endian if needed */);
	if (it != m_publishTracking->end()) {
		count = it->second;
		found = true;
	}

	if (increase) {
		count++;
	} else {
		count--;
	}

	if (count > 0) {
		(*m_publishTracking)[ip & 0xFFFFFF00] = count;
	} else if (found) {
		m_publishTracking->erase(ip & 0xFFFFFF00);
	} else {
		wxFAIL;
	}
#ifdef __DEBUG__
	if (!dbgReason.IsEmpty()) {
		AddDebugLogLineN(logKadEntryTracking,
			CFormat("%s %s (%s) - (%s), new count %u") % (increase ? "Adding" : "Removing") %
				KadIPToString(ip & 0xFFFFFF00) % KadIPToString(ip) % dbgReason % count);
	}
#endif
}

void CKeyEntry::MergeIPsAndFilenames(CKeyEntry *fromEntry)
{
	wxASSERT(!fromEntry || fromEntry->m_publishTracking == m_publishTracking);
	// Called when replacing a stored entry with a refreshed one: the tracked IPs and the
	// different filenames are taken over from the old entry, and the rest is overwritten with
	// the refreshed values. Not perfect for the taglist in some cases, but storing hundreds of
	// taglists to pick the best one -- as is done for the filenames -- is not affordable.
	if (m_publishingIPs !=
		NULL) { // This instance needs to be a new entry, otherwise we don't want/need to merge
		wxASSERT(fromEntry == NULL);
		wxASSERT(!m_publishingIPs->empty());
		wxASSERT(!m_filenames.empty());
		return;
	}

	// Fetch the AICH root hash this publisher reported, if any, and clear our own single-slot
	// list: from here on m_aichHashes is the *merged* list taken over from the stored entry,
	// and the reported hash is folded into it below so the popularity counts stay right.
	wxASSERT(m_aichHashes.GetSlotCount() <= 1);
	bool hasNewAICHHash = (m_aichHashes.GetSlotCount() > 0);
	CKadAICHHash newAICHHash = m_aichHashes.GetHashAt(0);
	m_aichHashes = CKadAICHHashList();

	bool refresh = false;
	if (fromEntry == NULL || fromEntry->m_publishingIPs == NULL) {
		wxASSERT(fromEntry == NULL);
		if (m_publishingIPs == NULL) {
			m_publishingIPs = new PublishingIPList();
		}
	} else {
		m_aichHashes = fromEntry->m_aichHashes;

		m_publishingIPs = fromEntry->m_publishingIPs;
		fromEntry->m_publishingIPs = NULL;
		bool fastRefresh = false;
		for (PublishingIPList::iterator it = m_publishingIPs->begin(); it != m_publishingIPs->end();
			++it) {
			if (it->m_ip == m_uIP) {
				refresh = true;
				if ((time(NULL) - it->m_lastPublish) < (KADEMLIAREPUBLISHTIMES - HR2S(1))) {
					AddDebugLogLineN(logKadEntryTracking,
						"FastRefresh publish, ip: " + KadIPToString(m_uIP));
					fastRefresh = true; // refreshed faster than expected, will not count
							    // into filenamepopularity index
				}
				it->m_lastPublish = time(NULL);

				// Has the AICH hash this publisher reports changed? A publisher
				// that stops reporting one (downgrade, or a hash set it can no
				// longer vouch for) must lose its vote, or the count would keep a
				// hash alive that nobody publishes any more.
				if (hasNewAICHHash) {
					if (it->m_aichHashIdx == CKadAICHHashList::INVALID_INDEX) {
						AddDebugLogLineN(logKadEntryTracking,
							"New AICH hash during publishing (publisher "
							"reported none before), publisher ip: " +
								KadIPToString(m_uIP));
						it->m_aichHashIdx = m_aichHashes.AddReference(newAICHHash);
					} else if (!(m_aichHashes.GetHashAt(it->m_aichHashIdx) ==
							   newAICHHash)) {
						AddDebugLogLineN(logKadEntryTracking,
							"AICH hash changed, publisher ip: " +
								KadIPToString(m_uIP));
						m_aichHashes.DropReferenceAt(it->m_aichHashIdx);
						it->m_aichHashIdx = m_aichHashes.AddReference(newAICHHash);
					}
				} else if (it->m_aichHashIdx != CKadAICHHashList::INVALID_INDEX) {
					AddDebugLogLineN(logKadEntryTracking,
						"AICH hash removed, publisher ip: " + KadIPToString(m_uIP));
					m_aichHashes.DropReferenceAt(it->m_aichHashIdx);
					it->m_aichHashIdx = CKadAICHHashList::INVALID_INDEX;
				}

				m_publishingIPs->push_back(*it);
				m_publishingIPs->erase(it);
				break;
			}
		}

		m_trustValue = fromEntry->m_trustValue;
		m_lastTrustValueCalc = fromEntry->m_lastTrustValueCalc;

		wxASSERT(m_filenames.size() ==
			 1); // we should have only one name here, since it's the entry from one single source
		sFileNameEntry currentName = { "", 0 };
		if (m_filenames.size() != 0) {
			currentName = m_filenames.front();
			m_filenames.pop_front();
		}

		// Cap m_filenames so a single CKeyEntry cannot accumulate unbounded filename
		// variants. A popular file collects one sFileNameEntry per distinct publisher-
		// chosen name -- renames, language variants, mirror prefixes, case differences --
		// and without a cap the list grows monotonically for the lifetime of the entry,
		// showing up as a steady ~MB/hour RSS climb in amuled. 100 matches the
		// m_publishingIPs cap below and is comfortably above any honest publisher set.
		const size_t MAX_FILENAMES = 100;

		// Compare-and-skip insertion: at the cap, a new entry is accepted only when its
		// popularity beats the weakest survivor, rather than pushing and then immediately
		// re-evicting. O(N) per insert via std::min_element.
		auto pushBounded = [&](const sFileNameEntry &candidate) {
			if (m_filenames.size() < MAX_FILENAMES) {
				m_filenames.push_back(candidate);
				return;
			}
			FileNameList::iterator weakest = std::min_element(m_filenames.begin(),
				m_filenames.end(),
				[](const sFileNameEntry &a, const sFileNameEntry &b) {
					return a.m_popularityIndex < b.m_popularityIndex;
				});
			if (candidate.m_popularityIndex > weakest->m_popularityIndex) {
				*weakest = candidate;
			}
			// else: candidate's popularity is no better than the
			// weakest already-kept entry; drop the candidate.
		};

		bool duplicate = false;
		for (FileNameList::iterator it = fromEntry->m_filenames.begin();
			it != fromEntry->m_filenames.end();
			++it) {
			sFileNameEntry nameToCopy = *it;
			// Defence-in-depth: SetFileName now rejects empty names, but an older
			// on-disk Kad index could still hold one from before that guard landed.
			if (nameToCopy.m_filename.IsEmpty()) {
				continue;
			}
			if (currentName.m_filename.CmpNoCase(nameToCopy.m_filename) == 0) {
				// the filename of our new entry matches with our old, increase the popularity
				// index for the old one
				duplicate = true;
				if (!fastRefresh) {
					nameToCopy.m_popularityIndex++;
				}
			}
			pushBounded(nameToCopy);
		}
		if (!duplicate && !currentName.m_filename.IsEmpty()) {
			// Skip the synthetic currentName = { "", 0 } default that happens when
			// m_filenames was unexpectedly empty above (wxASSERT fires in Debug,
			// Release keeps going).
			pushBounded(currentName);
		}
	}

	if (!refresh) {
		wxASSERT(m_uIP != 0);
		uint16_t aichHashIdx = hasNewAICHHash ? m_aichHashes.AddReference(newAICHHash)
						      : CKadAICHHashList::INVALID_INDEX;
		sPublishingIP add = { m_uIP, time(nullptr), aichHashIdx };
		m_publishingIPs->push_back(add);

		AdjustGlobalPublishTracking(m_uIP, true, "new publisher");

		// we keep track of max 100 IPs, in order to avoid too much time for
		// calculation/storing/loading.
		if (m_publishingIPs->size() > 100) {
			sPublishingIP curEntry = m_publishingIPs->front();
			m_publishingIPs->pop_front();
			m_aichHashes.DropReferenceAt(curEntry.m_aichHashIdx);
			AdjustGlobalPublishTracking(curEntry.m_ip, false, "more than 100 publishers purge");
		}

		ReCalculateTrustValue();
	}

	// Drop the slots no publisher points at any more and renumber the rest. DropReferenceAt()
	// above only zeroes a popularity count, so without this the list keeps every AICH hash the
	// entry has ever been told about: a publisher that rotates its hash each republish leaves a
	// slot behind every time, and they accumulate for the life of the process. The publisher
	// indexes are the only thing holding a slot, so they are remapped here in the same pass.
	const std::vector<uint16_t> compacted = m_aichHashes.Compact();
	for (auto &publisher : *m_publishingIPs) {
		if (publisher.m_aichHashIdx < compacted.size()) {
			publisher.m_aichHashIdx = compacted[publisher.m_aichHashIdx];
		} else {
			// INVALID_INDEX, or a stale index from a file written before the slot
			// ceiling existed. Either way it points at no hash.
			publisher.m_aichHashIdx = CKadAICHHashList::INVALID_INDEX;
		}
	}

	AddDebugLogLineN(logKadEntryTracking,
		CFormat("Indexed Keyword, Refresh: %s, Current Publisher: %s, Total Publishers: %u, Total "
			"different Names: %u, TrustValue: %.2f, file: %s") %
			(refresh ? "Yes" : "No") % KadIPToString(m_uIP) % m_publishingIPs->size() %
			m_filenames.size() % m_trustValue % m_uSourceID.ToHexString());
}

void CKeyEntry::ReCalculateTrustValue()
{
#define PUBLISHPOINTSSPERSUBNET 10.0
	// The trust value indicates how trustworthy or spammy this entry is. It lies between 0 and
	// ~10000, but the useful reading is that below 1 is bad and above 1 is good. It comes from
	// how many different IPs/24 published this entry and how many entries each of those IPs
	// has: each IP/24 has 3 points, so one IP publishing 3 entries alone gives each 3/3 = 1,
	// publishing 6 gives each 0.5, and a second publisher of one of them raises that one to 3/6
	// + 3/1 = 3.5.
	//
	// The point is to avoid being spammed for a given keyword by a small IP range, which would
	// otherwise blot out every other entry. If we index "Knoppix" and one IP publishes 500
	// variants of "knoppix casino 500% bonus.txt", those score 0.006 and are only returned
	// after everything above 1.
	//
	// Entries below 1 are NOT ignored or singled out; the rating only comes into play once
	// there are more results for a request than there is space for.
	wxCHECK_RET(m_publishingIPs != NULL, "No publishing IPs?");

	m_lastTrustValueCalc = ::GetTickCount64();
	m_trustValue = 0;
	wxASSERT(!m_publishingIPs->empty());
	for (PublishingIPList::iterator it = m_publishingIPs->begin(); it != m_publishingIPs->end(); ++it) {
		sPublishingIP curEntry = *it;
		uint32_t count = 0;
		GlobalPublishIPMap::const_iterator itMap = m_publishTracking->find(
			curEntry.m_ip & 0xFFFFFF00 /* /24 netmask, take care of endian if needed*/);
		if (itMap != m_publishTracking->end()) {
			count = itMap->second;
		}
		if (count > 0) {
			m_trustValue += PUBLISHPOINTSSPERSUBNET / count;
		} else {
			AddDebugLogLineN(logKadEntryTracking, "Inconsistency in RecalcualteTrustValue()");
			wxFAIL;
		}
	}
}

double CKeyEntry::GetTrustValue()
{
	// update if last calculation is too old, will assert if this entry is not supposed to have a
	// trustvalue
	if (::GetTickCount64() - m_lastTrustValueCalc > MIN2MS(10)) {
		ReCalculateTrustValue();
	}
	return m_trustValue;
}

void CKeyEntry::CleanUpTrackedPublishers()
{
	if (m_publishingIPs == NULL) {
		return;
	}

	time_t now = time(NULL);
	while (!m_publishingIPs->empty()) {
		sPublishingIP curEntry = m_publishingIPs->front();
		if (now - curEntry.m_lastPublish > KADEMLIAREPUBLISHTIMEK) {
			AdjustGlobalPublishTracking(curEntry.m_ip, false, "cleanup");
			// An expired publisher loses its AICH vote with everything else; without
			// this the hash would outlive every publisher that ever reported it and
			// keep being handed to searchers.
			m_aichHashes.DropReferenceAt(curEntry.m_aichHashIdx);
			m_publishingIPs->pop_front();
		} else {
			break;
		}
	}
}

void CKeyEntry::SetPublishedAICHHash(const CKadAICHHash &hash)
{
	m_aichHashes.AddReference(hash);
}

void CKeyEntry::WritePublishTrackingDataToFile(CFileDataIO *data, bool includesAICH)
{
	// format: <AICH_HashCount 2><{<AICH Hash 20>} AICH_HashCount>
	//         <Names_Count 4><{<Name string><PopularityIndex 4>} Names_Count>
	//         <PublisherCount 4><{<IP 4><Time 4><AICH Idx 2>} PublisherCount>
	//
	// Only referenced hashes are written, so the stored indexes are the compacted ones --
	// otherwise a hash whose last publisher expired would be reloaded with a popularity of zero
	// for ever.
	//
	// Gated together with the keyword-index version in CIndexed: with KadProtocol10
	// off we write a version-3 file with neither the AICH block nor the per-publisher
	// index, which is byte-for-byte what upstream writes and what an upstream binary
	// can read back.
	std::vector<uint16_t> newIndexes;
	if (includesAICH) {
		newIndexes = m_aichHashes.BuildCompactionMap();
		data->WriteUInt16(m_aichHashes.GetReferencedCount());
		for (uint16_t i = 0; i < m_aichHashes.GetSlotCount(); i++) {
			if (newIndexes[i] != CKadAICHHashList::INVALID_INDEX) {
				const CKadAICHHash &hash = m_aichHashes.GetHashAt(i);
				data->Write(hash.data(), hash.size());
			}
		}
	}

	data->WriteUInt32((uint32_t)m_filenames.size());
	for (FileNameList::const_iterator it = m_filenames.begin(); it != m_filenames.end(); ++it) {
		data->WriteString(it->m_filename, utf8strRaw, 2);
		data->WriteUInt32(it->m_popularityIndex);
	}

	if (m_publishingIPs != NULL) {
		data->WriteUInt32((uint32_t)m_publishingIPs->size());
		for (PublishingIPList::const_iterator it = m_publishingIPs->begin();
			it != m_publishingIPs->end();
			++it) {
			wxASSERT(it->m_ip != 0);
			data->WriteUInt32(it->m_ip);
			data->WriteUInt32((uint32_t)it->m_lastPublish);
			if (includesAICH) {
				uint16_t idx = CKadAICHHashList::INVALID_INDEX;
				if (it->m_aichHashIdx != CKadAICHHashList::INVALID_INDEX) {
					idx = newIndexes[it->m_aichHashIdx];
				}
				data->WriteUInt16(idx);
			}
		}
	} else {
		wxFAIL;
		data->WriteUInt32(0);
	}
}

void CKeyEntry::ReadPublishTrackingDataFromFile(
	CFileDataIO *data, bool includesAICH, const std::atomic<bool> *cancel)
{
	auto checkCancelled = [cancel]() {
		if (cancel && cancel->load()) {
			throw std::runtime_error("Kad index loading cancelled");
		}
	};
	checkCancelled();
	// format: <AICH_HashCount 2><{<AICH Hash 20>} AICH_HashCount>
	//         <Names_Count 4><{<Name string><PopularityIndex 4>} Names_Count>
	//         <PublisherCount 4><{<IP 4><Time 4><AICH Idx 2>} PublisherCount>
	//
	// The AICH block and the per-publisher index only exist from keyword-index
	// version 4 onwards; an older file loads with no hashes at all, which is
	// the same state as an entry only pre-0x09 peers ever published.
	wxASSERT(m_aichHashes.GetSlotCount() == 0);
	std::vector<CKadAICHHash> loadedHashes;
	if (includesAICH) {
		uint16_t hashCount = data->ReadUInt16();
		for (uint16_t i = 0; i < hashCount; i++) {
			checkCancelled();
			CKadAICHHash hash;
			data->Read(hash.data(), hash.size());
			loadedHashes.push_back(hash);
		}
	}

	wxASSERT(m_filenames.empty());
	uint32_t nameCount = data->ReadUInt32();
	for (uint32_t i = 0; i < nameCount; i++) {
		checkCancelled();
		sFileNameEntry toAdd;
		toAdd.m_filename = data->ReadString(true, 2);
		toAdd.m_popularityIndex = data->ReadUInt32();
		m_filenames.push_back(toAdd);
	}

	wxASSERT(m_publishingIPs == NULL);
	m_publishingIPs = new PublishingIPList();
	uint32_t ipCount = data->ReadUInt32();
#ifdef __WXDEBUG__
	uint32_t dbgLastTime = 0;
#endif
	for (uint32_t i = 0; i < ipCount; i++) {
		checkCancelled();
		sPublishingIP toAdd;
		toAdd.m_ip = data->ReadUInt32();
		wxASSERT(toAdd.m_ip != 0);
		toAdd.m_lastPublish = data->ReadUInt32();
#ifdef __WXDEBUG__
		wxASSERT(
			dbgLastTime <= (uint32_t)toAdd.m_lastPublish); // should always be sorted oldest first
		dbgLastTime = toAdd.m_lastPublish;
#endif

		// Re-attach this publisher to its AICH hash, rebuilding the popularity counts as we
		// go. An index pointing past the hashes we just read means a corrupt or truncated
		// file, so drop the hash rather than the whole entry.
		toAdd.m_aichHashIdx = CKadAICHHashList::INVALID_INDEX;
		if (includesAICH) {
			uint16_t storedIdx = data->ReadUInt16();
			if (storedIdx != CKadAICHHashList::INVALID_INDEX) {
				if (storedIdx >= loadedHashes.size()) {
					AddDebugLogLineC(logKadEntryTracking,
						"CKeyEntry::ReadPublishTrackingDataFromFile - out of range "
						"AICH hash index while loading keywords");
				} else {
					toAdd.m_aichHashIdx =
						m_aichHashes.AddReference(loadedHashes[storedIdx]);
				}
			}
		}

		AdjustGlobalPublishTracking(toAdd.m_ip, true, "");

		m_publishingIPs->push_back(toAdd);
	}
	ReCalculateTrustValue();
}

void CKeyEntry::DirtyDeletePublishData()
{
	// Publishers are removed rather than properly deleted with a global-map decrement; the
	// caller resets the global map anyway, and this speeds Kad shutdown up a little.
	delete m_publishingIPs;
	m_publishingIPs = NULL;
}

void CKeyEntry::WriteTagListWithPublishInfo(CFileDataIO *data)
{
	if (m_publishingIPs == NULL || m_publishingIPs->empty()) {
		wxFAIL;
		WriteTagList(data);
		return;
	}

	// A tag carrying this entry's publisher count, trust value and number of known names, as an
	// indicator of how valid the result is. Not trustworthy on its own -- we could be a bad
	// node -- but part of the puzzle.

	// One tag for TAG_PUBLISHINFO, plus TAG_KADAICHHASHRESULT if we have any AICH hash to
	// report. The AICH tag is written unconditionally rather than per requester because a Kad
	// search request carries no version byte: pre-0x09 peers skip the unknown tag, and it is
	// the *receiver* that gates on the sender's advertised version (see
	// CSearch::ProcessResultKeyword), so a fake tag from an old node cannot be laundered
	// through us.
	std::vector<uint8_t> aichTagValue;
	if (thePrefs::GetKadProtocol10()) {
		aichTagValue = m_aichHashes.EncodeResultTag();
	}
	WriteTagListInc(data, aichTagValue.empty() ? 1 : 2);

	uint32_t trust = (uint16_t)(GetTrustValue() * 100);
	uint32_t publishers = m_publishingIPs->size() & 0xFF /*% 256*/;
	uint32_t names = m_filenames.size() & 0xFF /*% 256*/;
	// 32 bit tag: <namecount uint8><publishers uint8><trustvalue*100 uint16>
	uint32_t tagValue = (names << 24) | (publishers << 16) | trust;
	data->WriteTag(CTagVarInt(TAG_PUBLISHINFO, tagValue));

	// Last, the AICH hashes reported for this file with the number of publishers behind each
	// one -- normally exactly one hash. A BSOB tag in Kad carries a uint8 length, and
	// CKadAICHHashList keeps the payload inside that budget.
	if (!aichTagValue.empty()) {
		wxASSERT(aichTagValue.size() <= 0xFF);
		data->WriteTag(
			CTagBsob(TAG_KADAICHHASHRESULT, &aichTagValue[0], (uint8_t)aichTagValue.size()));
	}
}
