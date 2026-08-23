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

#include "BrowseLifecycle.h"

namespace browse
{

Action Tick(const Record &rec, std::uint64_t now)
{
	if (rec.state != State::InProgress) {
		// Terminal already: whoever is tracking it should stop.
		return Action::Drop;
	}
	// Completion wins over expiry. A browse whose last listing landed in the
	// same tick its deadline passed succeeded; reporting that as a timeout
	// would throw away results we are holding.
	if (rec.outstanding == 0) {
		return Action::Complete;
	}
	if (rec.deadline != 0 && rec.deadline < now) {
		return Action::Expire;
	}
	return Action::None;
}

std::uint16_t BarValue(const Record &rec)
{
	if (rec.state != State::InProgress) {
		return 0xffff;
	}
	if (rec.totalDirs > 0) {
		int done = rec.totalDirs - (rec.outstanding > 0 ? rec.outstanding : 0);
		if (done < 0) {
			done = 0;
		} else if (done > rec.totalDirs) {
			done = rec.totalDirs;
		}
		return static_cast<std::uint16_t>(done * 100 / rec.totalDirs);
	}
	return 0;
}

Record ApplyAction(Record rec, Action action)
{
	if (rec.state != State::InProgress) {
		return rec;
	}
	switch (action) {
	case Action::Complete:
		rec.state = State::Finished;
		break;
	case Action::Expire:
		rec.state = State::Failed;
		break;
	case Action::None:
	case Action::Drop:
		break;
	}
	return rec;
}

Record OnDirectoryList(Record rec, int dirCount, std::uint64_t deadline)
{
	if (rec.state != State::InProgress) {
		// A late or duplicate answer for a browse that already ended changes
		// nothing; taking it would resurrect a terminal record.
		return rec;
	}
	rec.outstanding = dirCount > 0 ? dirCount : 0;
	rec.totalDirs = dirCount > 0 ? dirCount : 0;
	rec.deadline = deadline;
	return rec;
}

Record OnListingReceived(Record rec, std::uint64_t deadline)
{
	if (rec.state != State::InProgress) {
		return rec;
	}
	if (rec.outstanding == kFlatBrowse) {
		// The single answer a flat browse was waiting for.
		rec.outstanding = 0;
	} else if (rec.outstanding > 0) {
		--rec.outstanding;
	}
	rec.deadline = deadline;
	return rec;
}

} // namespace browse
