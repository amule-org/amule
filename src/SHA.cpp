//
// This file is part of the aMule Project.
//
// Copyright (c) 2004-2011 Angel Vidal ( kry@amule.org )
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

#include "SHA.h"

CSHA::CSHA()
{
	Reset();
}

void CSHA::Reset()
{
	m_hash = SHA1Digest();
	m_sha.Restart();
}

void CSHA::GetHash(SHA1Digest *pHash) const
{
	*pHash = m_hash;
}

void CSHA::Add(const void *pData, uint32 nLength)
{
	// Crypto++'s byte is always `unsigned char`; spell it that way rather
	// than CryptoPP::byte, which only exists from Crypto++ 5.6.5+ (issue
	// #449 — build failure on 5.6.4). unsigned char* binds to the byte*
	// API parameter on every Crypto++ version.
	m_sha.Update(static_cast<const unsigned char *>(pData), nLength);
}

void CSHA::Finish()
{
	m_sha.Final(m_hash.b);
}

void CSHA::GetHash(CAICHHash &rHash)
{
	wxASSERT(rHash.GetHashSize() == sizeof(SHA1Digest));
	GetHash((SHA1Digest *)rHash.GetRawHash());
}

void CSHA::Finish(CAICHHash &rHash)
{
	Finish();
	GetHash(rHash);
}

// File_checked_for_headers
