//								-*- C++ -*-
// This file is part of the aMule Project.
//
// Copyright (c) 2004-2011 Angel Vidal ( kry@amule.org )
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

#ifndef INDEXED_H
#define INDEXED_H

#include <functional>
#include <vector>

#include "SearchManager.h"
#include "Entry.h"
#include "../utils/BackgroundLoad.h"

class wxArrayString;
class CFile;

typedef std::list<Kademlia::CEntry *> CKadEntryPtrList;

struct Source
{
	Kademlia::CUInt128 sourceID;
	CKadEntryPtrList entryList;
};

typedef std::list<Source *> CKadSourcePtrList;
typedef std::map<Kademlia::CUInt128, Source *> CSourceKeyMap;

struct KeyHash
{
	Kademlia::CUInt128 keyID;
	CSourceKeyMap m_Source_map;
};

struct SrcHash
{
	Kademlia::CUInt128 keyID;
	CKadSourcePtrList m_Source_map;
};

struct Load
{
	Kademlia::CUInt128 keyID;
	uint32_t time;
};

struct SSearchTerm
{
	SSearchTerm();
	~SSearchTerm();

	enum ESearchTermType
	{
		AND,
		OR,
		NOT,
		String,
		MetaTag,
		OpGreaterEqual,
		OpLessEqual,
		OpGreater,
		OpLess,
		OpEqual,
		OpNotEqual
	} type;

	CTag *tag;
	wxArrayString *astr;

	SSearchTerm *left;
	SSearchTerm *right;
};

typedef std::map<Kademlia::CUInt128, KeyHash *> KeyHashMap;
typedef std::map<Kademlia::CUInt128, SrcHash *> SrcHashMap;
typedef std::map<Kademlia::CUInt128, Load *> LoadMap;

////////////////////////////////////////
namespace Kademlia
{
////////////////////////////////////////

class CKadUDPKey;

class CIndexed
{

public:
	CIndexed();
	CIndexed(const wxString &directory, const CUInt128 &kadID);
	using LoadState = CBackgroundLoad<std::unique_ptr<CIndexed>>::State;
	void ProcessIndexLoad();
	bool IsReady() const noexcept { return m_loadState == LoadState::Ready; }
	LoadState GetLoadState() const noexcept { return m_loadState; }
	CKeyEntry *CreateKeyEntry() { return new CKeyEntry(m_publishTracking); }
	~CIndexed();

	bool AddKeyword(const CUInt128 &keyWordID,
		const CUInt128 &sourceID,
		Kademlia::CKeyEntry *entry,
		uint8_t &load);
	bool AddSources(
		const CUInt128 &keyWordID, const CUInt128 &sourceID, Kademlia::CEntry *entry, uint8_t &load);
	bool AddNotes(
		const CUInt128 &keyID, const CUInt128 &sourceID, Kademlia::CEntry *entry, uint8_t &load);
	bool AddLoad(const CUInt128 &keyID, uint32_t time);
	size_t GetFileKeyCount() const noexcept { return m_Keyword_map.size(); }
	void SendValidKeywordResult(const CUInt128 &keyID,
		const SSearchTerm *pSearchTerms,
		uint32_t ip,
		uint16_t port,
		bool oldClient,
		uint16_t startPosition,
		const CKadUDPKey &senderKey);
	void SendValidSourceResult(const CUInt128 &keyID,
		uint32_t ip,
		uint16_t port,
		uint16_t startPosition,
		uint64_t fileSize,
		const CKadUDPKey &senderKey);
	void SendValidNoteResult(const CUInt128 &keyID,
		uint32_t ip,
		uint16_t port,
		uint64_t fileSize,
		const CKadUDPKey &senderKey);
	bool SendStoreRequest(const CUInt128 &keyID);
	uint32_t m_totalIndexSource;
	uint32_t m_totalIndexKeyword;
	uint32_t m_totalIndexNotes;
	uint32_t m_totalIndexLoad;

private:
	CIndexed(const wxString &directory, const CUInt128 &kadID, bool worker);
	std::shared_ptr<CKeyEntry::PublishTracking> m_publishTracking;
	std::unique_ptr<CBackgroundLoad<std::unique_ptr<CIndexed>>> m_loader;
	LoadState m_loadState;
	bool m_worker;
	CUInt128 m_kadID;
	enum class IndexPart
	{
		Load,
		Keyword,
		Source
	};
	std::vector<wxString> m_failedFiles;
	std::vector<wxString> m_preservedFiles;
	const wxString &PathOf(IndexPart part) const;
	void ClearPart(IndexPart part);
	void ReadLoadFile(const std::atomic<bool> &cancel);
	void ReadKeywordFile(const std::atomic<bool> &cancel);
	void ReadKeywordEntry(CFile &file,
		uint32_t version,
		const CUInt128 &keyID,
		const CUInt128 &sourceID,
		const std::atomic<bool> &cancel);
	void ReadSourceFile(const std::atomic<bool> &cancel);
	void ReadSourceEntry(CFile &file, const CUInt128 &keyID, const CUInt128 &sourceID);
	void SaveIndexFile(const wxString &path, const std::function<void(CFile &)> &write);
	void Clear();
	void WriteFile();
	time_t m_lastClean;
	KeyHashMap m_Keyword_map;
	SrcHashMap m_Sources_map;
	SrcHashMap m_Notes_map;
	LoadMap m_Load_map;
	wxString m_sfilename;
	wxString m_kfilename;
	wxString m_loadfilename;
	void ReadFile(const std::atomic<bool> &cancel);
	void Clean();
};

} // namespace Kademlia

#endif // INDEXED_H
// File_checked_for_headers
