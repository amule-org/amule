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

#ifndef PARTFILEWRITETHREAD_H
#define PARTFILEWRITETHREAD_H

#include "VolumeWritePool.h"
#include "PartFileWriteSession.h"
#include <memory>
#include <atomic>

#include <common/Path.h> // Needed for CPath

class CPartFile;
class CKnownFile;
class PartFileBufferedData;

// eMule ref: PartFile.h:104-108
// Part file buffer write status
#define PB_READY 0
#define PB_PENDING 1
#define PB_ERROR 2
#define PB_WRITTEN 3
// Not written: too little free space. The main thread puts it back and pauses the file.
#define PB_NOSPACE 4

// eMule ref: PartFileWriteThread.h:21-25
struct ToWrite
{
	CPartFile *pFile;
	PartFileBufferedData *pBuffer;
	//! For the free-space check, taken on the main thread when queued: the part file's
	//! actual data path and the user's limit, if it is on.
	CPath partPath;
	bool checkSpace;
	uint64 minFree;
	std::shared_ptr<PartFileWriteSession> session;
};

// Bounded asynchronous writer: one batch per discovered filesystem volume.
class CPartFileWriteThread
{
public:
	CPartFileWriteThread();
	~CPartFileWriteThread();

	void EndThread(); // eMule ref: PartFileWriteThread.cpp:62
	bool PrepareFile(CPartFile *file);
	bool QueueWrite(CPartFile *pFile, PartFileBufferedData *pBuffer, bool checkSpace, uint64 minFree);
	bool IsRunning() const { return m_pool != nullptr && m_running; }

	// Revoke queued ownership for a cancelling file. Active jobs retain their
	// pin until the provider call returns; CPartFile waits before closing/freeing.
	void DropReferencesTo(const CKnownFile *file);

private:
	static void Finish(ToWrite &write, uint8 status);
	void WriteBatch(std::vector<ToWrite> &batch);
	std::atomic<bool> m_running{ true };
	std::unique_ptr<VolumeWritePool<ToWrite>> m_pool;
	// QueueWrite/DropReferencesTo are main-thread operations; values are weak
	// so completed/deleted files are never kept alive by the registry.
	std::map<const CKnownFile *, std::weak_ptr<PartFileWriteSession>> m_sessions;
};

#endif // PARTFILEWRITETHREAD_H
// File_checked_for_headers
