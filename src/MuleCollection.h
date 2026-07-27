//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2007-2011 Johannes Krampf ( wuischke@amule.org )
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

#ifndef __MULECOLLECTION_H__
#define __MULECOLLECTION_H__

#include <istream>
#include <string>
#include <vector>

#include "Types.h"

/**
 * Reads an .emulecollection file and exposes the eD2k links it contains.
 *
 * Collection files are opened straight from a file manager, so the input is
 * arbitrary user data rather than something aMule wrote: every size read from
 * the file is bounded, every parse failure leaves the object empty, and the
 * links handed back are guaranteed to be well-formed single-line eD2k URIs.
 * That last point matters because callers feed them into the ED2KLinks IPC
 * file, where an embedded newline would let a collection inject extra lines.
 */
class CMuleCollection
{
private:
	std::vector<std::string> vCollection;

public:
	CMuleCollection() {};
	~CMuleCollection() {};

	/**
	 * Parses a collection held in memory. Both file-based overloads funnel
	 * through here; it is also the entry point the unit tests drive, since
	 * most interesting inputs are malformed and awkward to keep on disk.
	 *
	 * @return true if at least one usable link was found.
	 */
	bool OpenBuffer(const char *data, size_t len);

	/**
	 * Opens a collection by path.
	 *
	 * The narrow-string overload cannot represent every path on Windows and
	 * mangles non-ASCII ones under a non-UTF-8 locale on POSIX; it exists for
	 * the standalone `ed2k` helper, which is built without wxWidgets. Code
	 * that has a wxString should use that overload instead.
	 */
	bool Open(const std::string &File);
#ifndef USE_STD_STRING
	bool Open(const wxString &File);
#endif

	size_t size() const { return vCollection.size(); }
	std::string &operator[](size_t index) { return vCollection[index]; }
	const std::string &operator[](size_t index) const { return vCollection[index]; }

private:
	// Each parser fills a caller-supplied vector and is only allowed to
	// publish it on success, so a collection that goes bad halfway through
	// cannot leave partial results behind for the other parser to adopt.
	bool ParseBinary(std::istream &infile, std::vector<std::string> &out);
	bool ParseText(std::istream &infile, std::vector<std::string> &out);

	template <typename intType> intType ReadInt(std::istream &infile);

	std::string ReadString(std::istream &infile, int TagType);
};

#endif // __MULECOLLECTION_H__
