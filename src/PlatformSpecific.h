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

#ifndef PLATFORMSPECIFIC_H
#define PLATFORMSPECIFIC_H

#include <common/Path.h>
#include "Types.h"

namespace PlatformSpecific
{

/**
 * Creates @a name, sparse if the platform allows it, of @a size bytes. Returns whether the file was
 * created.
 */
bool CreateSparseFile(const CPath &name, uint64_t size);

/**
 * The max number of connections the current OS can handle. Anything but Windows returns the default
 * value (-1).
 */
#ifdef __WINDOWS__
int GetMaxConnections();
#else
inline int GetMaxConnections()
{
	return -1;
}
#endif

/**
 * File system types returned by GetFilesystemType.
 */
enum EFSType
{
	fsFAT,   //! File Allocation Table
	fsNTFS,  //! New Technology File System
	fsHFS,   //! Hierarchical File System
	fsHPFS,  //! High Performance File System
	fsMINIX, //! Minix file system
	fsOther  //! Unknown, other
};

/**
 * The filesystem type of @a path, or fsOther for an unknown or network file system, whose real type
 * cannot be determined.
 */
EFSType GetFilesystemType(const CPath &path);

/**
 * True if the filesystem at @a path can handle special chars such as ':' in file names. Always
 * false on MSW, since Windows cannot handle those characters on any file system. Based on
 * http://en.wikipedia.org/wiki/Comparison_of_file_systems
 */
#ifdef __WINDOWS__
inline bool CanFSHandleSpecialChars(const CPath &WXUNUSED(path))
{
	return false;
}
#else
// Other filesystem types may be added
inline bool CanFSHandleSpecialChars(const CPath &path)
{
	switch (GetFilesystemType(path)) {
	case fsFAT:
	case fsNTFS:
	case fsHFS:
		return false;
	default:
		return true;
	}
}
#endif

/**
 * True if the filesystem at @a path can handle large files (>4GB). Based on
 * http://en.wikipedia.org/wiki/Comparison_of_file_systems
 */
inline bool CanFSHandleLargeFiles(const CPath &path)
{
	switch (GetFilesystemType(path)) {
	case fsFAT:
	case fsHFS:
	case fsHPFS:
	case fsMINIX:
		return false;
	default:
		return true;
	}
}

/**
 * Disable or enable the computer's energy-saving "standby" mode.
 */
#if defined __WINDOWS__ || defined __WXMAC__
#define PLATFORMSPECIFIC_CAN_PREVENT_SLEEP_MODE 1
#else
#define PLATFORMSPECIFIC_CAN_PREVENT_SLEEP_MODE 0
#endif

void PreventSleepMode();
void AllowSleepMode();

}; /* namespace PlatformSpecific */

#endif /* PLATFORMSPECIFIC_H */
