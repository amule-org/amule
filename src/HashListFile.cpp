//
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

#include "HashListFile.h" // Interface declarations

#include <common/DataFileVersion.h>
#include "Preferences.h"
#include "CFile.h"
#include "Logger.h"
#include <common/Format.h>

CHashListFile::CHashListFile(const wxString &filename)
: m_filename(filename)
{
	Init();
}

bool CHashListFile::Init()
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
		if (version != CANCELEDFILE_VERSION) {
			AddLogLineC(CFormat(_("WARNING: %s might be corrupted (%i)")) % m_filename % version);
			return false;
		}

		uint32 RecordsNumber = file.ReadUInt32();
		AddDebugLogLineN(logKnownFiles,
			CFormat("Reading %i hashes from %s, format 0x%02x.") % RecordsNumber % m_filename %
				version);
		for (uint32 i = 0; i < RecordsNumber; i++) {
			CMD4Hash hash;
			file.Read(hash.GetHash(), 16);
			AddDebugLogLineN(logKnownFiles, CFormat("%s: read %s") % m_filename % hash.Encode());
			if (!hash.IsEmpty()) {
				m_hashes.insert(hash);
			}
		}
		AddDebugLogLineN(logKnownFiles, CFormat("Finished reading %s") % m_filename);

		return true;
	} catch (const CSafeIOException &e) {
		AddLogLineC(CFormat(_("IO error while reading %s file: %s")) % m_filename % e.what());
	}

	return false;
}

void CHashListFile::Save()
{
	CFile file(thePrefs::GetConfigDir() + m_filename, CFile::write);
	if (!file.IsOpened()) {
		return;
	}

	try {
		file.WriteUInt8(CANCELEDFILE_VERSION);
		file.WriteUInt32(m_hashes.size());
		for (const CMD4Hash &hash : m_hashes) {
			file.Write(hash.GetHash(), 16);
		}
		// Explicit so a buffered write error is caught here, not in the destructor.
		file.Close();
	} catch (const CIOFailureException &e) {
		AddLogLineC(CFormat(_("Error while saving %s file: %s")) % m_filename % e.what());
	}
}

bool CHashListFile::Contains(const CMD4Hash &hash) const
{
	return !hash.IsEmpty() && m_hashes.find(hash) != m_hashes.end();
}

bool CHashListFile::Add(const CMD4Hash &hash)
{
	return m_hashes.insert(hash).second;
}

bool CHashListFile::Remove(const CMD4Hash &hash)
{
	return m_hashes.erase(hash) > 0;
}

// File_checked_for_headers
