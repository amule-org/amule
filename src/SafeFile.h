//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2002-2011 Merkur ( devs@emule-project.net / http://www.emule-project.net )
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

#ifndef SAFEFILE_H
#define SAFEFILE_H

#include <wx/filename.h>      // Needed for wxFileName
#include <common/MuleDebug.h> // Needef for CMuleException
#include "Tag.h"

namespace Kademlia
{
class CUInt128;
}
using Kademlia::CUInt128;
class CMD4Hash;

/**
 * Interface for safe file IO.
 *
 * Basic IO operations either succeed or throw, so failure cannot be ignored. There are
 * three kinds of failure: read past EOF, errors while reading, and errors while
 * writing.
 *
 * Beyond basic IO, the interface reads and writes a number of simple data types, all
 * little-endian so they travel across platforms.
 *
 * Where an empty area is created -- by seeking past the end, then writing -- the value
 * of the bytes nothing was explicitly written to is unspecified.
 */
class CFileDataIO
{
public:
	/**
	 * Does nothing, but is needed to delete objects safely via CFileDataIO pointers.
	 */
	virtual ~CFileDataIO();

	/** Must return the current position in the file. */
	virtual uint64 GetPosition() const = 0;

	/** Must return the length of the file object in bytes. */
	virtual uint64 GetLength() const = 0;

	/** Returns true when the file position is past or at the end of the file. */
	virtual bool Eof() const;

	/**
	 * Changes the file position. Seeking to a negative position is illegal.
	 *
	 * @see wxFile::Seek
	 */
	virtual uint64 Seek(sint64 offset, wxSeekMode from = wxFromStart) const;

	/**
	 * Reads 'count' bytes into 'buffer'.
	 *
	 * @param buffer The target buffer.
	 * @param count The number of bytes to read.
	 *
	 * Reads exactly that many bytes unless it would read past the end of the file, in
	 * which case a CEOFException is thrown and the position and target buffer are left
	 * unchanged. A read can also fail on IO errors (bad hardware and the like), which
	 * throws a CIOFailureException.
	 */
	virtual void Read(void *buffer, size_t count) const;

	/**
	 * Write 'count' bytes from 'buffer' into the file.
	 *
	 * @param buffer The source-data buffer.
	 * @param count The number of bytes to write.
	 *
	 * Throws a CIOFailureException if it fails to write that many bytes -- hardware
	 * failure, lack of free space, and so on.
	 */
	virtual void Write(const void *buffer, size_t count);

	/**
	 * Reads the given type from the file, stored as little-endian.
	 *
	 * @see CSafeFileIO::Read
	 */
	//@{
	virtual uint8 ReadUInt8() const;
	virtual uint16 ReadUInt16() const;
	virtual uint32 ReadUInt32() const;
	virtual uint64 ReadUInt64() const;
	virtual CUInt128 ReadUInt128() const;
	virtual CMD4Hash ReadHash() const;
	virtual float ReadFloat() const;
	virtual unsigned char *ReadBsob(uint8 *size) const;
	//@}

	/**
	 * Reads a string from the file.
	 *
	 * @param bOptUTF8 Specifies if the string is UTF8 encoded.
	 * @param lenBytes The number of bytes used to store the string length.
	 * @param SafeRead Avoids throwing CEOFException, see below.
	 * @return The resulting text-string.
	 *
	 * With SafeRead set, CSafeFileIO crops the length read from the length field (see
	 * lenBytes), so at most GetLength() - GetPosition() bytes are read.
	 *
	 * @see CSafeFileIO::Read
	 */
	virtual wxString ReadString(bool bOptUTF8, uint8 lenBytes = 2, bool SafeRead = false) const;

	/**
	 * Reads a string from the file, where the length is specified directly. Typically
	 * used when the text field's length is not stored as an integer field in front of
	 * the text field.
	 *
	 * @param bOptUTF8 Specifies if the string is UTF8 encoded.
	 * @param length The length of the string.
	 * @return The resulting text-string.
	 */
	virtual wxString ReadOnlyString(bool bOptUTF8, uint16 length) const;

	/**
	 * Writes a value of the given type to the file, storing it as little-endian.
	 *
	 * @see CSafeFileIO::Write
	 */
	//@{
	virtual void WriteUInt8(uint8 value);
	virtual void WriteUInt16(uint16 value);
	virtual void WriteUInt32(uint32 value);
	virtual void WriteUInt64(uint64 value);
	virtual void WriteUInt128(const CUInt128 &value);
	virtual void WriteHash(const CMD4Hash &value);
	virtual void WriteFloat(float value);
	virtual void WriteBsob(const unsigned char *val, uint8 size);
	//@}

	/**
	 * Writes a text string to the file.
	 *
	 * @param str The string to be written.
	 * @param encoding The text-encoding, see EUtf8Str.
	 * @param lenBytes The number of bytes used to store the string length: 0 (no length
	 * field), 2 or 4.
	 *
	 * @see CSafeFileIO::Write
	 */
	virtual void WriteString(const wxString &str, EUtf8Str encoding = utf8strNone, uint8 lenBytes = 2);

	/* Warning: Special Kad functions, needs documentation */

	CTag *ReadTag(bool bOptACP = false) const;
	void ReadTagPtrList(TagPtrList *taglist, bool bOptACP = false) const;

	void WriteTag(const CTag &tag);
	void WriteTagPtrList(const TagPtrList &tagList);

	/* Special ED2Kv2 function */
	uint64 GetIntTagValue() const;

	/* Some functions I added for simplicity */
	// Very obvious
	bool IsEmpty() { return (GetLength() == 0); }

	// Appends to the end
	void Append(const uint8 *buffer, int n)
	{
		Seek(0, wxFromEnd);
		Write(buffer, n);
	}

protected:
	/**
	 * The actual read / write function, as implemented by subclasses.
	 *
	 * @param buffer The buffer to read data into / write data from.
	 * @param count The number of bytes to read / write.
	 * @return The number of bytes actually read / written, or -1 on error -- the caller
	 * uses the return value to decide whether the operation succeeded.
	 *
	 * Must not throw either CSafeIOException; CSafeFileIO::Read and CSafeFileIO::Write
	 * do that.
	 */
	//@{
	virtual sint64 doRead(void *buffer, size_t count) const = 0;
	virtual sint64 doWrite(const void *buffer, size_t count) = 0;
	//@}

	/**
	 * The actual seek function, as implemented by subclasses.
	 *
	 * @param offset The absolute offset to seek to.
	 * @return The resulting offset.
	 *
	 * Must not throw a CSafeIOException; CSafeFileIO::Seek handles that. A failed seek
	 * is currently a fatal error.
	 */
	virtual sint64 doSeek(sint64 offset) const = 0;

private:
	/**
	 * Does the actual writing of the string.
	 *
	 * @param str The string to be written.
	 * @param encoding The encoding of the string.
	 * @param lenBytes The number of bytes used to store the string length.
	 */
	void WriteStringCore(const char *str, EUtf8Str encoding, uint8 lenBytes);
};

/**
 * Base class of the IO exceptions the CSafeFileIO interface and its implementations
 * use.
 */
struct CSafeIOException : public CMuleException
{
	CSafeIOException(const wxString &type, const wxString &desc);
};

/**
 * Thrown on an attempt to read past the end of the file. Typically an invalid packet
 * that is shorter than expected, and not fatal.
 */
struct CEOFException : public CSafeIOException
{
	CEOFException(const wxString &desc);
};

/**
 * A failure in the basic read and write operations: thrown when a read or a write
 * moves fewer than the specified number of bytes.
 */
struct CIOFailureException : public CSafeIOException
{
	CIOFailureException(const wxString &type, const wxString &desc);
	CIOFailureException(const wxString &desc);
};

#endif // SAFEFILE_H
// File_checked_for_headers
