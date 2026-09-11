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

#ifndef MEMFILE_H
#define MEMFILE_H

#include "SafeFile.h" // Needed for CFileDataIO

/**
 * Virtual files stored in memory.
 *
 * Allows binary data in memory, such as data sent over networks, to be manipulated. Over writing
 * the stream onto a struct it gains: contents that can be read dynamically for various versions of
 * the same packet, transparent endian correction (CMemFile converts to and from little-endian, so
 * no explicit conversions are needed), and strings of dynamic length. Most of that holds for
 * writing packets too.
 *
 * @see CFileDataIO
 */
class CMemFile : public CFileDataIO
{
public:
	/**
	 * Creates a dynamic file object.
	 *
	 * @param growthRate How much the buffer grows by when the memfile runs out of space. A
	 * larger value cuts down reallocations at the cost of slightly higher memory use. If the
	 * size of the whole file is known in advance, pass it as the growth rate to avoid needless
	 * reallocations. Zero makes the memfile allocate exactly the amount needed and no more.
	 */
	CMemFile(unsigned int growthRate = 1024);

	/**
	 * Creates a mem-file attached to an already existing buffer.
	 *
	 * @param buffer A pre-existing buffer.
	 * @param bufferSize The size of the buffer.
	 *
	 * The buffer is assumed to already contain data, so the file size is set to match its size.
	 * Resizing to a length between zero and 'bufferSize' is valid; resizing past it, or writing
	 * past it, is not. The buffer is _not_ freed by CMemFile on destruction. A const uint8*
	 * buffer makes the memfile read-only.
	 */
	CMemFile(uint8 *buffer, size_t bufferSize);
	CMemFile(const uint8 *buffer, size_t bufferSize);

	/** Destructor. */
	virtual ~CMemFile();

	/** @see CFileDataIO::GetPosition */
	virtual uint64 GetPosition() const;

	/** @see CFileDataIO::GetLength */
	virtual uint64 GetLength() const;

	/**
	 * Changes the length of the file, possibly resizing the buffer. If the current position is
	 * past @a newLen, it is moved to the end of the file. Growing a file with an attached
	 * buffer past the buffer's actual size is illegal.
	 */
	virtual void SetLength(size_t newLen);

	/**
	 * Resets the memfile to the start.
	 */
	virtual void Reset() const { doSeek(0); }

	/**
	 * Bytes available to read before EOF.
	 */
	virtual sint64 GetAvailable() const { return GetLength() - GetPosition(); }

	/**
	 * Resets the memfile to the starting values.
	 */
	virtual void ResetData();

	// Sometimes it's useful to get the buffer and do stuff with it.
	uint8 *GetRawBuffer() const { return m_buffer; }

protected:
	/** @see CFileDataIO::doRead */
	virtual sint64 doRead(void *buffer, size_t count) const;

	/** @see CFileDataIO::doWrite */
	virtual sint64 doWrite(const void *buffer, size_t count);

	/** @see CFileDataIO::doSeek */
	virtual sint64 doSeek(sint64 offset) const;

private:
	//! A CMemFile is neither copyable nor assignable.
	//@{
	CMemFile(const CMemFile &);
	CMemFile &operator=(const CMemFile &);
	//@}

	/** Enlarges the buffer to at least 'size' length. */
	void enlargeBuffer(size_t size);

	//! The growth-rate for the buffer.
	unsigned int m_growthRate;
	//! The current position in the file.
	mutable size_t m_position;
	//! The actual size of the buffer.
	size_t m_BufferSize;
	//! The size of the virtual file, may be less than the buffer-size.
	size_t m_fileSize;
	//! If true, the buffer will be freed upon termination.
	bool m_delete;
	//! read-only mark.
	bool m_readonly;
	//! The actual buffer.
	uint8 *m_buffer;
};

#endif // MEMFILE_H
// File_checked_for_headers
