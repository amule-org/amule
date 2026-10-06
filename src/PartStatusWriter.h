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

#ifndef PARTSTATUSWRITER_H
#define PARTSTATUSWRITER_H

#include "SafeFile.h"              // Needed for CFileDataIO
#include "VerifyLocalDataResult.h" // Needed for CVerifyLocalDataResult

// The part status of OP_FILESTATUS and OP_REASKACK. Header-only so the wire format can be
// tested: CKnownFile and CPartFile reach theApp and cannot be linked into a unit test.

// The part count, then one bit per part, LSB first, the last byte padded with zeros. Peers reject
// a count other than their GetED2KPartCount().
template <typename HasPart> void WritePartBitmap(CFileDataIO &out, uint16 parts, HasPart hasPart)
{
	out.WriteUInt16(parts);
	uint16 done = 0;
	while (done != parts) {
		uint8 towrite = 0;
		for (uint32 i = 0; i != 8 && done != parts; ++i, ++done) {
			if (hasPart(done)) {
				towrite |= (1 << i);
			}
		}
		out.WriteUInt8(towrite);
	}
}

// A complete file: count 0 ("every part"), or the bitmap without the parts a check found corrupt.
inline void WriteCompleteFilePartStatus(
	CFileDataIO &out, const CVerifyLocalDataResult &result, uint16 ed2kParts)
{
	if (!result.IsCorrupt()) {
		out.WriteUInt16(0);
		return;
	}
	WritePartBitmap(out, ed2kParts, [&result](uint16 part) { return !result.IsPartCorrupt(part); });
}

#endif // PARTSTATUSWRITER_H
