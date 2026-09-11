//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2009-2011 Frediano Ziglio (freddy77@gamilc.com)
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

#ifndef FILEAREA_H
#define FILEAREA_H

#include "Types.h" // Needed for uint8_t

class CFileAreaSigHandler;
class CFileAutoClose;

/**
 * Optimizes file read/write using mapped memory where supported.
 */
class CFileArea
{
	friend class CFileAreaSigHandler;

public:
	/**
	 * Creates an uninitialized file area.
	 */
	CFileArea();

	/**
	 * Closes the file if opened.
	 */
	virtual ~CFileArea();

	/**
	 * Closes the file.
	 */
	bool Close();

	/**
	 * Initialises the buffer with @a count bytes of @a file from @a offset. The buffer is a
	 * memory-mapped area or an allocated one, depending on the system.
	 */
	void ReadAt(CFileAutoClose &file, uint64 offset, size_t count);

	/**
	 * Starts a new write.
	 */
	void StartWriteAt(CFileAutoClose &file, uint64 offset, size_t count);

	/**
	 * Flushes data not yet written.
	 */
	bool FlushAt(CFileAutoClose &file, uint64 offset, size_t count);

	/**
	 * The buffer holding the data read or to be written, or NULL if not initialized.
	 */
	uint8_t *GetBuffer() const { return m_buffer; };

	/**
	 * Reports a pending error.
	 */
	void CheckError();

	/**
	 * Runtime switch for the mmap file-I/O path (the MMapEnabled preference). Read once per
	 * operation at ReadAt()/StartWriteAt() time; an area already opened keeps its mode (see
	 * m_mmap_buffer), so flipping this mid-transfer only affects later operations and is safe
	 * with active downloads and uploads. No-op on builds without MMAP_SUPPORTED.
	 */
	static void SetMMapEnabled(bool enabled);
	static bool GetMMapEnabled();

private:
	//! A CFileArea is neither copyable nor assignable.
	//@{
	CFileArea(const CFileArea &);
	CFileArea &operator=(const CFileArea &);
	//@}

	/**
	 * Buffer used for read/write operations: points inside m_mmap_buffer when mapped, otherwise
	 * at an allocated buffer to be freed.
	 */
	uint8_t *m_buffer;
	/**
	 * Memory mapped area, or NULL if not mapped.
	 */
	uint8_t *m_mmap_buffer;
	/**
	 * Length of the mapped region, currently used only for munmap.
	 */
	size_t m_length;
	/**
	 * Global chain.
	 */
	CFileArea *m_next;
	/**
	 * File handle to release.
	 */
	CFileAutoClose *m_file;
	/**
	 * True if an error was detected.
	 */
	bool m_error;
};

#endif // FILEAREA_H
// File_checked_for_headers
