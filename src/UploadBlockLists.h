//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
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
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
//

#ifndef UPLOADBLOCKLISTS_H
#define UPLOADBLOCKLISTS_H

#include <cstddef>
#include <list>

#include "OtherStructs.h" // Needed for Requested_Block_Struct

// How many sent blocks a client's done list keeps. AddReqBlock walks the whole list for every
// block a peer requests, and a session that is never kicked used to keep every block it sent.
// A duplicate request can only repeat a block still in flight, and peers keep at most 24 blocks
// in flight (aMule's BDP cap; eMule's is 18).
constexpr std::size_t kMaxDoneBlocks = 64;

// Adds a block just sent to the front of the done list and frees the oldest past the limit.
inline void PushDoneBlock(std::list<Requested_Block_Struct *> &done, Requested_Block_Struct *block)
{
	done.push_front(block);
	while (done.size() > kMaxDoneBlocks) {
		delete done.back();
		done.pop_back();
	}
}

#endif // UPLOADBLOCKLISTS_H
// File_checked_for_headers
