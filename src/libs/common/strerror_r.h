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

#ifndef MULE_STRERROR_R_H
#define MULE_STRERROR_R_H

/**
 * Return a string describing an error number.
 *
 * Implements the XSI-compliant strerror_r() wherever possible, and is thread safe if a thread-safe
 * function to get the error description exists.
 *
 * @param errnum Error number for which the description is needed.
 * @param buf    Buffer to store the error description.
 * @param buflen Length of the buffer.
 * @return 0 on success; -1 on error, with errno set to indicate it.
 */
extern "C" int mule_strerror_r(int errnum, char *buf, size_t buflen);

#endif /* MULE_STRERROR_R_H */
