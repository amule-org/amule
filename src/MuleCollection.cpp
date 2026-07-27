//
// This file is part of the aMule Project.
//
// Copyright (c) 2007-2011 Johannes Krampf ( wuischke@amule.org )
//
// Other code by:
//
// Angel Vidal Veiga aka Kry <kry@amule.org>
// * changed class names
//
// Marcelo Malheiros <mgmalheiros@gmail.com>
// * fixed error with FT_FILEHASH
// * added initial 5 tag/file support
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

#include "MuleCollection.h"

#include <cstring>
#include <fstream>
#include <sstream>

#ifndef USE_STD_STRING
#include <wx/file.h>
#endif

namespace
{
/**
 * Upper bound on a collection file we are willing to read into memory.
 * The format caps itself at 1024 entries, so anything this large is either
 * corrupt or not a collection at all.
 */
const size_t MAX_COLLECTION_BYTES = 4 * 1024 * 1024;

/**
 * Upper bound on a single length-prefixed blob inside the header. Without
 * it a twelve-byte crafted file can ask us to allocate 4 GiB.
 */
const uint32_t MAX_BLOB_BYTES = 64 * 1024;

/**
 * Rejects anything that would break the one-link-per-line contract the
 * ED2KLinks file relies on, or the pipe-delimited eD2k URI grammar itself.
 * Filenames are UTF-8, so the comparison has to be done on unsigned chars -
 * with a signed char every byte from 0x80 up would look like a control
 * character and legitimate non-ASCII names would be thrown away.
 */
bool IsSafeLinkField(const std::string &field)
{
	for (const char ch : field) {
		const unsigned char c = static_cast<unsigned char>(ch);
		if (c < 0x20 || c == 0x7F || c == '|') {
			return false;
		}
	}
	return true;
}

/**
 * True when the buffer opens with a collection version header. Caller has
 * already checked that at least four bytes are available.
 */
bool LooksLikeBinary(const char *data)
{
	uint32_t version = 0;
	memcpy(&version, data, sizeof(version));
	// TODO: byte-sex, as in ReadInt().
	return version == 0x01 || version == 0x02;
}
} // namespace

bool CMuleCollection::OpenBuffer(const char *data, size_t len)
{
	vCollection.clear();

	if (data == nullptr || len == 0) {
		return false;
	}

	// Skip a UTF-8 byte-order mark. Text collections are hand-made lists of
	// links, and a Windows editor saves them with a BOM by default; without
	// this the first link fails its "starts with ed2k://|file|" check and a
	// single-entry collection looks empty. A binary collection never starts
	// with one, so this only ever affects the text form.
	if (len >= 3 && static_cast<unsigned char>(data[0]) == 0xEF &&
		static_cast<unsigned char>(data[1]) == 0xBB && static_cast<unsigned char>(data[2]) == 0xBF) {
		data += 3;
		len -= 3;
		if (len == 0) {
			return false;
		}
	}

	const std::string buffer(data, len);
	std::istringstream infile(buffer, std::ios::in | std::ios::binary);

	// The two formats are told apart by the leading four bytes and only one
	// parser ever runs. A binary collection starts with a version of 1 or 2;
	// a text one starts with "ed2k://", which as a little-endian uint32 is
	// 0x6b326465 and so can never be mistaken for a version.
	//
	// Never try the text parser as a fallback for a binary file. The text
	// parser scans for anything that looks like a link, so a hostile
	// collection could smuggle one inside a filename field, have the binary
	// parser correctly reject the entry, and still see it harvested from the
	// raw bytes on the second pass.
	std::vector<std::string> parsed;
	const bool isBinary = (len >= sizeof(uint32_t)) && (LooksLikeBinary(data));

	if (isBinary) {
		if (ParseBinary(infile, parsed) && !parsed.empty()) {
			vCollection.swap(parsed);
			return true;
		}
		return false;
	}

	if (ParseText(infile, parsed) && !parsed.empty()) {
		vCollection.swap(parsed);
		return true;
	}

	return false;
}

bool CMuleCollection::Open(const std::string &File)
{
	std::ifstream infile;

	infile.open(File.c_str(), std::ifstream::in | std::ifstream::binary);
	if (!infile.is_open()) {
		return false;
	}

	infile.seekg(0, std::ios::end);
	const std::streamoff length = infile.tellg();
	if (length <= 0 || length > static_cast<std::streamoff>(MAX_COLLECTION_BYTES)) {
		return false;
	}
	infile.seekg(0, std::ios::beg);

	std::vector<char> buffer(static_cast<size_t>(length));
	infile.read(&buffer[0], static_cast<std::streamsize>(length));
	if (!infile) {
		return false;
	}

	return OpenBuffer(&buffer[0], buffer.size());
}

#ifndef USE_STD_STRING
bool CMuleCollection::Open(const wxString &File)
{
	// wxFile is what makes this overload worth having: it opens wide paths
	// on Windows and converts with wxConvFileName on POSIX, where handing a
	// locale-narrowed path to std::ifstream silently fails to open.
	wxFile file;
	if (!file.Open(File, wxFile::read)) {
		return false;
	}

	const wxFileOffset length = file.Length();
	if (length <= 0 || length > static_cast<wxFileOffset>(MAX_COLLECTION_BYTES)) {
		return false;
	}

	std::vector<char> buffer(static_cast<size_t>(length));
	if (file.Read(&buffer[0], buffer.size()) != static_cast<ssize_t>(buffer.size())) {
		return false;
	}

	return OpenBuffer(&buffer[0], buffer.size());
}
#endif // !USE_STD_STRING

template <typename intType> intType CMuleCollection::ReadInt(std::istream &infile)
{
	intType integer = 0;
	infile.read(reinterpret_cast<char *>(&integer), sizeof(intType));
	// TODO: byte-sex
	return integer;
}

std::string CMuleCollection::ReadString(std::istream &infile, int TagType)
{
	if (TagType >= 0x11 && TagType <= 0x20) {
		std::vector<char> buffer(TagType - 0x10);
		infile.read(&buffer[0], TagType - 0x10);
		return buffer.empty() ? std::string() : std::string(buffer.begin(), buffer.end());
	}
	if (TagType == 0x02) {
		uint16_t TagStringSize = ReadInt<uint16_t>(infile);
		if (TagStringSize == 0) {
			return std::string();
		}
		std::vector<char> buffer(TagStringSize);
		infile.read(&buffer[0], TagStringSize);
		return std::string(buffer.begin(), buffer.end());
	}
	return std::string();
}

bool CMuleCollection::ParseBinary(std::istream &infile, std::vector<std::string> &out)
{
	uint32_t cVersion = ReadInt<uint32_t>(infile);

	if (!infile.good() || (cVersion != 0x01 && cVersion != 0x02)) {
		return false;
	}

	uint32_t hTagCount = ReadInt<uint32_t>(infile);
	if (!infile.good() || hTagCount > 3) {
		return false;
	}

	for (uint32_t hTi = 0; hTi < hTagCount; hTi++) {
		int hTagType = infile.get();

		// hTagFormat == 1 -> FT-value is given
		uint16_t hTagFormat = ReadInt<uint16_t>(infile);
		if (hTagFormat != 0x0001) {
			return false;
		}

		int hTag = infile.get();
		if (!infile.good()) {
			return false;
		}
		switch (hTag) {
		// FT_FILENAME
		case 0x01: {
			/*std::string fileName =*/ReadString(infile, hTagType);
			break;
		}
		// FT_COLLECTIONAUTHOR
		case 0x31: {
			/*std::string CollectionAuthor =*/ReadString(infile, hTagType);
			break;
		}
		// FT_COLLECTIONAUTHORKEY
		case 0x32: {
			uint32_t hTagBlobSize = ReadInt<uint32_t>(infile);
			// The size is straight from the file, so bound it before
			// it becomes an allocation - otherwise twelve crafted
			// bytes ask for 4 GiB and the bad_alloc escapes Open().
			if (!infile.good() || hTagBlobSize > MAX_BLOB_BYTES) {
				return false;
			}
			if (hTagBlobSize > 0) {
				std::vector<char> CollectionAuthorKey(hTagBlobSize);
				infile.read(&CollectionAuthorKey[0], hTagBlobSize);
			}
			break;
		}
		// UNDEFINED TAG
		default:
			// An unknown header tag carries an unknown payload, so we
			// cannot skip past it and every subsequent read would be
			// misaligned. Bail rather than parse garbage - the same
			// choice the per-file tag loop below already makes.
			return false;
		}
		if (!infile.good()) {
			return false;
		}
	}

	uint32_t cFileCount = ReadInt<uint32_t>(infile);

	/*
	softlimit is set to 1024 to avoid problems with big uint32_t values
	I don't believe anyone would want to use an emulecollection file
	to store more than 1024 files, but just raise below value in case
	you know someone who does.
	*/

	if (!infile.good() || cFileCount > 1024) {
		return false;
	}

	out.reserve(cFileCount);

	for (uint32_t cFi = 0; cFi < cFileCount; ++cFi) {
		uint32_t fTagCount = ReadInt<uint32_t>(infile);

		if (!infile.good() || fTagCount > 6) {
			return false;
		}

		std::string fileHash = std::string(32, '0');
		bool haveHash = false;
		uint64_t fileSize = 0;
		std::string fileName;
		std::string rootHash;
		for (uint32_t fTi = 0; fTi < fTagCount; ++fTi) {
			int fTagType = infile.get();
			if (!infile.good()) {
				return false;
			}

			int fTag = infile.get();
			if (!infile.good()) {
				return false;
			}

			switch (fTag) {
			// FT_FILEHASH
			case 0x28: {
				std::vector<char> bFileHash(16);
				infile.read(&bFileHash[0], 16);
				if (!infile.good()) {
					return false;
				}
				const std::string hex = "0123456789abcdef";
				for (int pos = 0; pos < 16; pos++) {
					const unsigned char b = static_cast<unsigned char>(bFileHash[pos]);
					fileHash[pos * 2] = hex[(b >> 4) & 0x0F];
					fileHash[(pos * 2) + 1] = hex[b & 0x0F];
				}
				haveHash = true;
				break;
			}
			// FT_AICH_FILEHASH
			case 0x27: {
				rootHash = ReadString(infile, 0x02);
				break;
			}
			// FT_FILESIZE
			case 0x02: {
				switch (fTagType) {
				case 0x83: {
					fileSize = ReadInt<uint32_t>(infile);
					break;
				}
				case 0x88: {
					fileSize = ReadInt<uint16_t>(infile);
					break;
				}
				case 0x89: {
					fileSize = infile.get();
					break;
				}
				case 0x8b: {
					fileSize = ReadInt<uint64_t>(infile);
					break;
				}
				default: // Invalid file structure
					return false;
				}
				break;
			}
			// FT_FILENAME
			case 0x01: {
				fileName = ReadString(infile, fTagType ^ 0x80);
				break;
			}
			// FT_FILECOMMENT
			case 0xF6: {
				/* std::string FileComment =*/ReadString(infile, fTagType ^ 0x80);
				break;
			}
			// FT_FILERATING
			case 0xF7: {
				if (fTagType == 0x89) { // TAGTYPE_UINT8
							// uint8_t FileRating =
					infile.get();

				} else {
					return false;
				}
				break;
			}
			// UNDEFINED TAG
			default:
				return false;
			}
			if (!infile.good()) {
				return false;
			}
		}

		// Without a hash the synthesised link is syntactically valid but
		// names the all-zero file id, which queues a download that can
		// never find a source or complete. Skip the entry instead.
		//
		// The field checks keep a hostile filename from carrying a
		// newline (which would forge extra lines once the link reaches
		// the ED2KLinks file) or a pipe (which would forge extra fields).
		if (!fileName.empty() && fileSize > 0 && haveHash && IsSafeLinkField(fileName)) {
			if (!rootHash.empty() && !IsSafeLinkField(rootHash)) {
				rootHash.clear();
			}
			std::stringstream link;
			// ed2k://|file|fileName|fileSize|fileHash|/
			link << "ed2k://|file|" << fileName << "|" << fileSize << "|" << fileHash;
			if (!rootHash.empty()) {
				link << "|h=" << rootHash;
			}
			link << "|/";
			out.push_back(link.str());
		}
	}

	return true;
}

bool CMuleCollection::ParseText(std::istream &infile, std::vector<std::string> &out)
{
	std::string line;

	while (getline(infile, line, (char)10 /* LF */)) {
		// An empty line used to underflow the length arithmetic here and
		// index far past the end of the string, which threw out of
		// Open() uncaught. Blank lines are ordinary in hand-written
		// collections, and a corrupt binary file falls through to this
		// parser and produces them routinely.
		if (!line.empty() && (char)13 /* CR */ == line[line.size() - 1]) {
			line.erase(line.size() - 1);
		}
		if (line.size() > 50 && line.compare(0, 13, "ed2k://|file|") == 0 &&
			line.compare(line.size() - 2, 2, "|/") == 0) {
			out.push_back(line);
		}
	}

	return !out.empty();
}
