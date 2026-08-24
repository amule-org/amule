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

#include "SearchFile.h" // Interface declarations.

#include <tags/FileTags.h>

#include "amule.h" // Needed for theApp
#include "CanceledFileList.h"
#include "MemFile.h"     // Needed for CMemFile
#include "Preferences.h" // Needed for thePrefs
#include "GuiEvents.h"
#include "Logger.h"
#include "PartFile.h"      // Needed for CPartFile::CanAddSource
#include "DownloadQueue.h" // Needed for CDownloadQueue
#include "KnownFileList.h" // Needed for CKnownFileList

CSearchFile::CSearchFile(const CMemFile &data,
	bool optUTF8,
	wxUIntPtr searchID,
	uint32_t serverIP,
	uint16_t serverPort,
	const wxString &directory,
	bool kademlia)
: m_parent(NULL)
, m_showChildren(false)
, m_searchID(searchID)
, m_sourceCount(0)
, m_completeSourceCount(0)
, m_kademlia(kademlia)
, m_downloadStatus(NEW)
, m_directory(directory)
, m_clientServerIP(serverIP)
, m_clientServerPort(serverPort)
, m_kadPublishInfo(0)
{
	m_abyFileHash = data.ReadHash();
	SetDownloadStatus();
	m_clientID = data.ReadUInt32();
	m_clientPort = data.ReadUInt16();

	if (!m_clientID || !m_clientPort || !IsGoodIP(m_clientID, thePrefs::FilterLanIPs())) {
		m_clientID = 0;
		m_clientPort = 0;
	}

	uint32 tagcount = data.ReadUInt32();
	for (unsigned int i = 0; i < tagcount; ++i) {
		CTag tag(data, optUTF8);
		switch (tag.GetNameID()) {
		case FT_FILENAME:
			SetFileName(CPath(tag.GetStr()));
			break;
		case FT_FILESIZE:
			SetFileSize(tag.GetInt());
			break;
		case FT_FILESIZE_HI:
			SetFileSize((((uint64)tag.GetInt()) << 32) + GetFileSize());
			break;
		case FT_FILERATING:
			if (kademlia) {
				// Kad results carry the publisher's rating raw (0-5); there is no
				// server-side packing. Clamp defensively against malformed values.
				m_iUserRating = static_cast<int8>(std::min<uint64>(tag.GetInt(), 5));
			} else {
				// ed2k servers relay the rating packed (low byte = rating * 51); the
				// low nibble recovers rating * 3, hence the /3.
				m_iUserRating = (tag.GetInt() & 0xF) / 3;
			}
			break;
		case FT_SOURCES:
			m_sourceCount = tag.GetInt();
			break;
		case FT_COMPLETE_SOURCES:
			m_completeSourceCount = tag.GetInt();
			break;
		case FT_PERMISSIONS:
		case FT_KADLASTPUBLISHKEY:
		case FT_PARTFILENAME:
			// Just ignore
			break;
		default:
			AddTagUnique(tag);
		}
	}

	if (!GetFileName().IsOk()) {
		throw CInvalidPacket("No filename in search result");
	}
}

// CECID() below deliberately allocates a fresh EC ID instead of copying other's.
CSearchFile::CSearchFile(const CSearchFile &other) // NOLINT(bugprone-copy-constructor-init)
: CAbstractFile(other)
, CECID() // create a new ID for now
, m_parent(other.m_parent)
, m_showChildren(other.m_showChildren)
, m_searchID(other.m_searchID)
, m_sourceCount(other.m_sourceCount)
, m_completeSourceCount(other.m_completeSourceCount)
, m_kademlia(other.m_kademlia)
, m_downloadStatus(other.m_downloadStatus)
, m_directory(other.m_directory)
, m_clients(other.m_clients)
, m_clientID(other.m_clientID)
, m_clientPort(other.m_clientPort)
, m_clientServerIP(other.m_clientServerIP)
, m_clientServerPort(other.m_clientServerPort)
, m_kadPublishInfo(other.m_kadPublishInfo)
{
	for (size_t i = 0; i < other.m_children.size(); ++i) {
		m_children.push_back(new CSearchFile(*other.m_children.at(i)));
	}
}

// Only reachable from LoadFromFile(); every field gets overwritten from the
// stream except m_downloadStatus, which stays NEW until SetDownloadStatus()
// recomputes it against the live knownfiles/downloadqueue/canceledfiles --
// see the WriteToFile()/LoadFromFile() comments in SearchFile.h.
CSearchFile::CSearchFile()
: m_parent(nullptr)
, m_showChildren(false)
, m_searchID(0)
, m_sourceCount(0)
, m_completeSourceCount(0)
, m_kademlia(false)
, m_downloadStatus(NEW)
, m_clientID(0)
, m_clientPort(0)
, m_clientServerIP(0)
, m_clientServerPort(0)
, m_kadPublishInfo(0)
{
}

CSearchFile::~CSearchFile()
{
	// Let any open comments dialog drop its pointer before we free the object
	// (a Kad-notes lookup can still be showing this result). Fired for children
	// too — they are deleted just below.
	Notify_SearchFileBeingDestroyed(this);

	for (size_t i = 0; i < m_children.size(); ++i) {
		delete m_children.at(i);
	}
}

bool CSearchFile::WriteToFile(CFileDataIO *file) const
{
	file->WriteHash(m_abyFileHash);

	// Fixed tags: filename, size, sources, complete-sources, and rating if
	// ever set. Anything else already riding in m_taglist (e.g. Kad-relayed
	// extras AddTagUnique kept) is appended after, same shape as
	// CKnownFile::WriteToFile. Unlike CKnownFile's on-the-wire pairing of a
	// FT_FILESIZE_HI tag alongside a 32-bit FT_FILESIZE (a server-wire
	// idiom, see KnownFile.cpp:1327 -- two 32-bit tags used INSTEAD OF a
	// 64-bit one, never alongside it), this format always writes the size
	// as a single 64-bit-capable tag: CTagIntSized already stores the whole
	// value, so a size-hi tag here would be additive rather than
	// complementary on read.
	uint32 tagcount = 4;
	if (m_iUserRating != 0) {
		tagcount++;
	}
	tagcount += m_taglist.size();
	file->WriteUInt32(tagcount);

	CTagString nametag(FT_FILENAME, CPath::ToUniv(GetFileName()));
	nametag.WriteTagToFile(file);

	CTagIntSized sizetag(FT_FILESIZE, GetFileSize(), 64);
	sizetag.WriteTagToFile(file);

	CTagInt32 sourcestag(FT_SOURCES, m_sourceCount);
	sourcestag.WriteTagToFile(file);

	CTagInt32 completesourcestag(FT_COMPLETE_SOURCES, m_completeSourceCount);
	completesourcestag.WriteTagToFile(file);

	if (m_iUserRating != 0) {
		CTagInt32 ratingtag(FT_FILERATING, m_iUserRating);
		ratingtag.WriteTagToFile(file);
	}

	for (const CTag &tag : m_taglist) {
		tag.WriteTagToFile(file);
	}

	// Non-tag fields.
	file->WriteUInt8(m_kademlia ? 1 : 0);
	file->WriteString(m_directory, utf8strRaw); // Always UTF8 -- matches LoadFromFile's ReadString(true)
	file->WriteUInt32(m_clientID);
	file->WriteUInt16(m_clientPort);
	file->WriteUInt32(m_clientServerIP);
	file->WriteUInt16(m_clientServerPort);
	file->WriteUInt32(m_kadPublishInfo);

	file->WriteUInt16((uint16)m_clients.size());
	for (const ClientStruct &client : m_clients) {
		file->WriteUInt32(client.m_ip);
		file->WriteUInt16(client.m_port);
		file->WriteUInt32(client.m_serverIP);
		file->WriteUInt16(client.m_serverPort);
	}

	file->WriteUInt16((uint16)m_children.size());
	for (const CSearchFile *child : m_children) {
		if (!child->WriteToFile(file)) {
			return false;
		}
	}

	return true;
}

CSearchFile *CSearchFile::LoadFromFile(CFileDataIO *file, bool allowChildren)
{
	std::unique_ptr<CSearchFile> result(new CSearchFile());

	result->m_abyFileHash = file->ReadHash();

	uint32 tagcount = file->ReadUInt32();
	for (uint32 i = 0; i != tagcount; ++i) {
		CTag tag(*file, true);
		switch (tag.GetNameID()) {
		case FT_FILENAME:
			result->SetFileName(CPath::FromUniv(tag.GetStr()));
			break;
		case FT_FILESIZE:
			result->SetFileSize(tag.GetInt());
			break;
		case FT_SOURCES:
			result->m_sourceCount = tag.GetInt();
			break;
		case FT_COMPLETE_SOURCES:
			result->m_completeSourceCount = tag.GetInt();
			break;
		case FT_FILERATING:
			result->m_iUserRating = static_cast<int8>(tag.GetInt());
			break;
		default:
			result->AddTagUnique(tag);
		}
	}

	if (!result->GetFileName().IsOk()) {
		return nullptr;
	}

	result->m_kademlia = file->ReadUInt8() != 0;
	result->m_directory = file->ReadString(true);
	result->m_clientID = file->ReadUInt32();
	result->m_clientPort = file->ReadUInt16();
	result->m_clientServerIP = file->ReadUInt32();
	result->m_clientServerPort = file->ReadUInt16();
	result->m_kadPublishInfo = file->ReadUInt32();

	uint16 clientcount = file->ReadUInt16();
	for (uint16 i = 0; i != clientcount; ++i) {
		uint32_t ip = file->ReadUInt32();
		uint16_t port = file->ReadUInt16();
		uint32_t serverIP = file->ReadUInt32();
		uint16_t serverPort = file->ReadUInt16();
		result->m_clients.emplace_back(ip, port, serverIP, serverPort);
	}

	uint16 childcount = file->ReadUInt16();
	if (childcount > 0 && !allowChildren) {
		// A real result tree is two levels deep at most -- parent plus
		// alternative-filename children, never grandchildren (the same
		// invariant AddChild() enforces at runtime: "A child cannot have
		// children of its own"). A child record claiming children of its
		// own is malformed; without this check a crafted/corrupt file
		// nesting one child per level recurses unbounded and overflows
		// the stack before any other validation gets a chance to run.
		return nullptr;
	}
	for (uint16 i = 0; i != childcount; ++i) {
		CSearchFile *child = LoadFromFile(file, false);
		if (!child) {
			return nullptr;
		}
		// Not AddChild(): that method also does live-search duplicate
		// merging (matching filenames get combined, see above), which
		// doesn't apply to restoring an already-finalized tree -- every
		// child here was already distinct when written.
		child->m_parent = result.get();
		result->m_children.push_back(child);
	}

	// m_downloadStatus is deliberately left at its NEW default here --
	// recomputing it needs theApp->downloadqueue/knownfiles/canceledfiles,
	// which is the caller's job (see the header comment on LoadFromFile)
	// so this stays a pure parser callable before those singletons exist.
	return result.release();
}

// SearchFile.cpp is core-only (CORE_SOURCES); the amulegui build compiles its
// CSearchFile methods in amule-remote-gui.cpp, where the CLIENT_GUI version of
// GetRatingAndComments (returning the EC-streamed list) also lives.
void CSearchFile::GetRatingAndComments(FileRatingList &list) const
{
	// A search result has no connected sources, so its comments are exactly the
	// on-demand Kad notes gathered by the CAbstractFile helper.
	list.clear();
	GetKadNotesComments(list);
}

void CSearchFile::AddClient(const ClientStruct &client)
{
	for (std::list<ClientStruct>::const_iterator it = m_clients.begin(); it != m_clients.end(); ++it) {
		if (client.m_ip == it->m_ip && client.m_port == it->m_port)
			return;
	}
	m_clients.push_back(client);
}

void CSearchFile::MergeResults(const CSearchFile &other)
{
	// Sources
	if (m_kademlia) {
		m_sourceCount = std::max(m_sourceCount, other.m_sourceCount);
		m_completeSourceCount = std::max(m_completeSourceCount, other.m_completeSourceCount);
	} else {
		m_sourceCount += other.m_sourceCount;
		m_completeSourceCount += other.m_completeSourceCount;
	}

	// Publish info
	if (m_kadPublishInfo == 0) {
		if (other.m_kadPublishInfo != 0) {
			m_kadPublishInfo = other.m_kadPublishInfo;
		}
	} else {
		if (other.m_kadPublishInfo != 0) {
			m_kadPublishInfo =
				std::max(m_kadPublishInfo & 0xFF000000, other.m_kadPublishInfo & 0xFF000000) |
				std::max(m_kadPublishInfo & 0x00FF0000, other.m_kadPublishInfo & 0x00FF0000) |
				(((m_kadPublishInfo & 0x0000FFFF) + (other.m_kadPublishInfo & 0x0000FFFF)) >>
					1);
		}
	}

	// Rating
	if (m_iUserRating != 0) {
		if (other.m_iUserRating != 0) {
			m_iUserRating = (m_iUserRating + other.m_iUserRating) / 2;
		}
	} else {
		if (other.m_iUserRating != 0) {
			m_iUserRating = other.m_iUserRating;
		}
	}

	// copy possible available sources from new result
	if (other.GetClientID() && other.GetClientPort()) {
		// pre-filter sources which would be dropped by CPartFile::AddSources
		if (CPartFile::CanAddSource(other.GetClientID(),
			    other.GetClientPort(),
			    other.GetClientServerIP(),
			    other.GetClientServerPort())) {
			CSearchFile::ClientStruct client(other.GetClientID(),
				other.GetClientPort(),
				other.GetClientServerIP(),
				other.GetClientServerPort());
			AddClient(client);
		}
	}
}

void CSearchFile::AddChild(CSearchFile *file)
{
	wxCHECK_RET(file, "Not a valid child!");
	wxCHECK_RET(!file->GetParent(), "Search-result can only be child of one other result");
	wxCHECK_RET(!file->HasChildren(), "Result already has children, cannot become child.");
	wxCHECK_RET(!GetParent(), "A child cannot have children of its own");
	wxCHECK_RET(GetFileHash() == file->GetFileHash(), "Mismatching child/parent hashes");
	wxCHECK_RET(GetFileSize() == file->GetFileSize(), "Mismatching child/parent sizes");

	// If no children exists, then we add the current item.
	if (GetChildren().empty()) {
		// Merging duplicate names instead of adding a new one
		if (file->GetFileName() == GetFileName()) {
			AddDebugLogLineN(logSearch, CFormat("Merged results for '%s'") % GetFileName());
			MergeResults(*file);
			delete file;
			return;
		} else {
			// The first child will always be the first result we received.
			AddDebugLogLineN(
				logSearch, CFormat("Created initial child for result '%s'") % GetFileName());
			m_children.push_back(new CSearchFile(*this));
			m_children.back()->m_parent = this;
			// Announced like any other new row. Without this the group is
			// formed with two children while only the incoming one is ever
			// notified, so a view that builds its rows from notifications --
			// rather than re-reading the model as it draws, which is what
			// hides this on macOS -- shows a two-variant group holding one
			// row, and the one missing is the result received first.
			Notify_Search_Add_Result(m_children.back());
		}
	}

	file->m_parent = this;

	for (size_t i = 0; i < m_children.size(); ++i) {
		CSearchFile *other = m_children.at(i);
		// Merge duplicate filenames
		if (other->GetFileName() == file->GetFileName()) {
			other->MergeResults(*file);
			UpdateParent();
			delete file;
			return;
		}
	}

	// New unique child.
	m_children.push_back(file);
	UpdateParent();

	if (ShowChildren()) {
		Notify_Search_Add_Result(file);
	}
}

void CSearchFile::UpdateParent()
{
	wxCHECK_RET(!m_parent, "UpdateParent called on child item");

	uint32_t sourceCount = 0;         // ed2k: sum of all sources, kad: the max sources found
	uint32_t completeSourceCount = 0; // ed2k: sum of all sources, kad: the max sources found
	uint32_t differentNames = 0;      // max known different names
	uint32_t publishersKnown = 0;     // max publishers known
	uint32_t trustValue = 0;          // average trust value
	unsigned publishInfoTags = 0;
	unsigned ratingCount = 0;
	unsigned ratingTotal = 0;
	CSearchResultList::const_iterator best = m_children.begin();
	for (CSearchResultList::const_iterator it = m_children.begin(); it != m_children.end(); ++it) {
		const CSearchFile *child = *it;

		// Locate the most common name
		if (child->GetSourceCount() > (*best)->GetSourceCount()) {
			best = it;
		}

		// Sources
		if (m_kademlia) {
			sourceCount = std::max(sourceCount, child->m_sourceCount);
			completeSourceCount = std::max(completeSourceCount, child->m_completeSourceCount);
		} else {
			sourceCount += child->m_sourceCount;
			completeSourceCount += child->m_completeSourceCount;
		}

		// Publish info
		if (child->GetKadPublishInfo() != 0) {
			differentNames =
				std::max(differentNames, (child->GetKadPublishInfo() & 0xFF000000) >> 24);
			publishersKnown =
				std::max(publishersKnown, (child->GetKadPublishInfo() & 0x00FF0000) >> 16);
			trustValue += child->GetKadPublishInfo() & 0x0000FFFF;
			publishInfoTags++;
		}

		// Rating
		if (child->HasRating()) {
			ratingCount++;
			ratingTotal += child->UserRating();
		}

		// Available sources
		if (child->GetClientID() && child->GetClientPort()) {
			CSearchFile::ClientStruct client(child->GetClientID(),
				child->GetClientPort(),
				child->GetClientServerIP(),
				child->GetClientServerPort());
			AddClient(client);
		}
		for (std::list<ClientStruct>::const_iterator cit = child->m_clients.begin();
			cit != child->m_clients.end();
			++cit) {
			AddClient(*cit);
		}
	}

	m_sourceCount = sourceCount;
	m_completeSourceCount = completeSourceCount;

	if (publishInfoTags > 0) {
		m_kadPublishInfo = ((differentNames & 0x000000FF) << 24) |
				   ((publishersKnown & 0x000000FF) << 16) |
				   ((trustValue / publishInfoTags) & 0x0000FFFF);
	} else {
		m_kadPublishInfo = 0;
	}

	if (ratingCount > 0) {
		m_iUserRating = ratingTotal / ratingCount;
	} else {
		m_iUserRating = 0;
	}

	SetFileName((*best)->GetFileName());
}

void CSearchFile::SetDownloadStatus()
{
	bool isPart = theApp->downloadqueue->GetFileByID(m_abyFileHash) != NULL;
	bool isKnown = theApp->knownfiles->FindKnownFileByID(m_abyFileHash) != NULL;
	bool isCanceled = theApp->canceledfiles->IsCanceledFile(m_abyFileHash);

	if (isCanceled && isPart) {
		m_downloadStatus = QUEUEDCANCELED;
	} else if (isCanceled) {
		m_downloadStatus = CANCELED;
	} else if (isPart) {
		m_downloadStatus = QUEUED;
	} else if (isKnown) {
		m_downloadStatus = DOWNLOADED;
	} else {
		m_downloadStatus = NEW;
	}
	// Update status of children too
	for (CSearchResultList::iterator it = m_children.begin(); it != m_children.end(); ++it) {
		Notify_Search_Update_Sources(*it);
	}
}

// File_checked_for_headers
