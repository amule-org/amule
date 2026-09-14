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

#ifndef SHA_H
#define SHA_H

#include "SHAHashSet.h"
#include "CryptoPP_Inc.h"

#define SHA1_DIGEST_SIZE 20

typedef struct
{
	uint8 b[SHA1_DIGEST_SIZE];
} SHA1Digest;

class CSHA : public CAICHHashAlgo
{

	// Construction

public:
	CSHA();
	virtual ~CSHA() {};
	// Operations

public:
	virtual void Reset();
	virtual void Add(const void *pData, uint32 nLength);
	virtual void Finish(CAICHHash &Hash);
	virtual void GetHash(CAICHHash &Hash);
	void GetHash(SHA1Digest *pHash) const;
	void Finish();

private:
	CryptoPP::SHA1 m_sha;
	SHA1Digest m_hash;
};

#endif // SHA_H
// File_checked_for_headers
