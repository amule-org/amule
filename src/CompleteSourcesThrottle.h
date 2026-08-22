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

#ifndef COMPLETESOURCESTHROTTLE_H
#define COMPLETESOURCESTHROTTLE_H

#include <cstdint>

/**
 * Whether an otherwise-throttled complete-sources recompute must run anyway.
 *
 * CKnownFile::UpdatePartsInfo() only recomputes every 60 s. One transition
 * cannot wait that out: the upload list going empty. Every caller is driven by
 * a peer event and there is no periodic sweep, so if the last requesting peer
 * leaves inside the window, the throttled call is the last one that file will
 * ever get and the counts keep their values for the life of the process.
 *
 * Non-zero is asked of all three exported fields, not just the scalar. They do
 * not move together: Hi is a percentile of the peers' self-reported counts and
 * is only ever floored at the scalar, never tied to it, so a mixed population
 * -- the norm, since a peer without extended requests v2 contributes 0 --
 * settles at scalar 0 with Hi non-zero. Guarding on the scalar alone let that
 * Hi through, and the desktop column and Web UI detail panel both render
 * "< Hi" whenever Lo is 0, so the file kept claiming sources it no longer had
 * (issue #1065). Lo cannot diverge from the scalar, but it is included so the
 * guard stays honest if the estimation is ever changed.
 *
 * Still false once all three have settled at 0, which is what stops an idle
 * shared file re-entering the recompute on every later call.
 *
 * Free function in a header of its own so it can be unit-tested: CKnownFile
 * itself reaches theApp and cannot be linked into a test.
 */
inline bool CompleteSourcesNeedRecompute(
	bool uploadListEmpty, std::uint16_t count, std::uint16_t countLo, std::uint16_t countHi)
{
	return uploadListEmpty && (count != 0 || countLo != 0 || countHi != 0);
}

#endif // COMPLETESOURCESTHROTTLE_H
// File_checked_for_headers
