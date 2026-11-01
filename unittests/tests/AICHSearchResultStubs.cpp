//								-*- C++ -*-
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
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

// Application dependencies of the production SHAHashSet.cpp.
// Consensus tests must never enter client recovery; fail at that boundary.
#include <muleunit/test.h>
#include <cstdio>
#include <cstdlib>
#include <amule.h>
#include <Preferences.h>
#include <DownloadQueue.h>
#include <PartFile.h>
#include <updownclient.h>
#include <SearchList.h>
#include <Packet.h>

using namespace muleunit;

CamuleDaemonApp *theApp = nullptr;
bool CPreferences::s_AICHTrustEveryHash = false;
wxString CPreferences::s_configDir;

void CClientRef::Unlink()
{
	ASSERT_TRUE(m_client == nullptr);
}

CClientRef::CClientRef(const CClientRef &other)
: m_client(nullptr)
{
	ASSERT_TRUE(other.m_client == nullptr);
}

void CUpDownClient::SetReqFileAICHHash(CAICHHash *)
{
	FAIL_M("AICH consensus test unexpectedly entered client recovery");
}

bool CDownloadQueue::IsPartFile(const CKnownFile *) const
{
	FAIL_M("AICH consensus test unexpectedly queried the download queue");
	return false;
}

void CPartFile::RequestAICHRecovery(uint16)
{
	FAIL_M("AICH consensus test unexpectedly requested recovery");
}

wxString CUpDownClient::GetClientFullInfo()
{
	FAIL_M("AICH consensus test unexpectedly inspected a recovery client");
	return wxString();
}

// Search model tests do not query live queues, clients, or Kad notes. UI
// notifications are intentionally ignored by this headless fixture.
#include <KnownFileList.h>
#include <HashListFile.h>

std::atomic<uint32> CECID::s_IDCounter{ 0 };
bool CPreferences::s_filterLanIP = false;

bool IsGoodIP(uint32, bool) noexcept
{
	std::fputs("Search model test unexpectedly validated a client endpoint\n", stderr);
	std::abort();
}

void CAbstractFile::GetKadNotesComments(FileRatingList &) const
{
	FAIL_M("Search model test unexpectedly queried Kad notes");
}

void CAbstractFile::GetRatingAndComments(FileRatingList &) const
{
	FAIL_M("Search model test unexpectedly queried comments");
}

bool CPartFile::CanAddSource(uint32, uint16, uint32, uint16, uint8 *, bool, bool)
{
	FAIL_M("Search model test unexpectedly added a download source");
	return false;
}

CPartFile *CDownloadQueue::GetFileByID(const CMD4Hash &) const
{
	FAIL_M("Search model test unexpectedly queried the download queue");
	return nullptr;
}

bool CKnownFileList::WasKnown(const CMD4Hash &)
{
	FAIL_M("Search model test unexpectedly queried known files");
	return false;
}

bool CHashListFile::Contains(const CMD4Hash &) const
{
	FAIL_M("Search model test unexpectedly queried canceled files");
	return false;
}

// Declared here, not through GuiEvents.h, to keep its dependencies out of this test.
namespace MuleNotify
{
class CMuleNotiferBase;
void SearchFileBeingDestroyed(CSearchFile *);
void Search_Add_Result(CSearchFile *);
void Search_Update_Sources(CSearchFile *);
void HandleNotification(const CMuleNotiferBase &);

void SearchFileBeingDestroyed(CSearchFile *) {}
void Search_Add_Result(CSearchFile *) {}
void Search_Update_Sources(CSearchFile *) {}
void HandleNotification(const CMuleNotiferBase &) {}
} // namespace MuleNotify

// Define the real classes' virtual anchors so the compiler emits their genuine
// vtables and hierarchy RTTI. UBSan's vptr instrumentation references this even
// in application branches that ownerless consensus never enters. Every stub
// remains fail-fast; no fake typeinfo symbols or sanitizer exclusions are used.
namespace
{
[[noreturn]] void UnexpectedApplicationCall(const char *method) noexcept
{
	std::fprintf(stderr, "AICH fixture unexpectedly called %s\n", method);
	std::abort();
}
} // namespace

CDownloadQueue::~CDownloadQueue()
{
	UnexpectedApplicationCall("CDownloadQueue::~CDownloadQueue");
}
CSearchList::~CSearchList()
{
	UnexpectedApplicationCall("CSearchList::~CSearchList");
}
wxBEGIN_EVENT_TABLE(CSearchList, wxEvtHandler)
wxEND_EVENT_TABLE()

CKnownFile::~CKnownFile()
{
	UnexpectedApplicationCall("CKnownFile::~CKnownFile");
}
CPartFile::~CPartFile()
{
	UnexpectedApplicationCall("CPartFile::~CPartFile");
}

void CDownloadQueue::ObserverAdded(ObserverType *)
{
	UnexpectedApplicationCall("CDownloadQueue::ObserverAdded");
}

void CKnownFile::LoadComment() const
{
	UnexpectedApplicationCall("CKnownFile::LoadComment");
}
void CKnownFile::SetFileSize(uint64)
{
	UnexpectedApplicationCall("CKnownFile::SetFileSize");
}
void CKnownFile::SetFileName(const CPath &)
{
	UnexpectedApplicationCall("CKnownFile::SetFileName");
}
bool CKnownFile::LoadFromFile(const CFileDataIO *)
{
	UnexpectedApplicationCall("CKnownFile::LoadFromFile");
}
CPacket *CKnownFile::CreateSrcInfoPacket(const CUpDownClient *, uint8, uint16)
{
	UnexpectedApplicationCall("CKnownFile::CreateSrcInfoPacket");
}
void CKnownFile::UpdatePartsInfo()
{
	UnexpectedApplicationCall("CKnownFile::UpdatePartsInfo");
}
wxString CKnownFile::GetFeedback() const
{
	UnexpectedApplicationCall("CKnownFile::GetFeedback");
}
void CKnownFile::SetHashingProgress(uint16) const
{
	UnexpectedApplicationCall("CKnownFile::SetHashingProgress");
}
void CKnownFile::WritePartStatus(CMemFile *)
{
	UnexpectedApplicationCall("CKnownFile::WritePartStatus");
}

void CPartFile::SetFileName(const CPath &)
{
	UnexpectedApplicationCall("CPartFile::SetFileName");
}
uint8 CPartFile::GetStatus(bool) const
{
	UnexpectedApplicationCall("CPartFile::GetStatus");
}
void CPartFile::UpdatePartsInfo()
{
	UnexpectedApplicationCall("CPartFile::UpdatePartsInfo");
}
wxString CPartFile::GetFeedback() const
{
	UnexpectedApplicationCall("CPartFile::GetFeedback");
}
CPacket *CPartFile::CreateSrcInfoPacket(const CUpDownClient *, uint8, uint16)
{
	UnexpectedApplicationCall("CPartFile::CreateSrcInfoPacket");
}
void CPartFile::SetHashingProgress(uint16) const
{
	UnexpectedApplicationCall("CPartFile::SetHashingProgress");
}
void CPartFile::UpdateFileRatingCommentAvail()
{
	UnexpectedApplicationCall("CPartFile::UpdateFileRatingCommentAvail");
}
void CPartFile::GetRatingAndComments(FileRatingList &) const
{
	UnexpectedApplicationCall("CPartFile::GetRatingAndComments");
}
void CPartFile::WritePartStatus(CMemFile *)
{
	UnexpectedApplicationCall("CPartFile::WritePartStatus");
}

SearchType CSearchList::GetSearchLifecycleKindById(wxUIntPtr) const
{
	FAIL_M("AICH consensus test unexpectedly queried search lifecycle");
	return LocalSearch;
}
