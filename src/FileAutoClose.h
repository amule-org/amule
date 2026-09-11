//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2008-2011 Stu Redman ( sturedman@amule.org )
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

#ifndef FILEAUTOCLOSE_H
#define FILEAUTOCLOSE_H

#include "CFile.h" // Needed for CFile

/**
 * Wraps CFile so the file handle can be closed and reopened on use, keeping the number of open
 * handles down.
 */
class CFileAutoClose
{
public:
	/**
	 * Creates a closed file.
	 */
	CFileAutoClose();

	/**
	 * Calls Open on the specified file. Check IsOpened() to see whether it succeeded.
	 */
	CFileAutoClose(const CPath &path, CFile::OpenMode mode = CFile::read);

	/**
	 * Request auto closing of the file handle. @a now true closes immediately, false closes
	 * when the timeout expires. True if the file has been, or already was, autoclosed.
	 */
	bool Release(bool now = false);

	/**
	 * Opens the file at @a path in @a mode (see CFile). True if it was opened.
	 */
	bool Open(const CPath &path, CFile::OpenMode mode = CFile::read);

	/**
	 * Equivalent to Open with OpenMode 'write'. @a overwrite says whether an existing target
	 * file should be overwritten. @see CFile::Open
	 */
	bool Create(const CPath &path, bool overwrite = false);

	/**
	 * Closes the file. Calling this on a closed file is illegal.
	 */
	bool Close();

	/**
	 * @see CSafeFileIO::GetLength. Calling this on a closed file is illegal.
	 */
	uint64 GetLength() const;

	/**
	 * Resizes the file to the specified length.
	 */
	bool SetLength(uint64 newLength);

	/**
	 * The path of the currently opened file.
	 */
	const CPath &GetFilePath() const;

	/**
	 * True if the file is opened.
	 */
	bool IsOpened() const;

	/**
	 * Reads @a count bytes into @a buffer from @a offset in the file. See CFileDataIO::Read.
	 */
	void ReadAt(void *buffer, uint64 offset, size_t count);

	/**
	 * Writes @a count bytes from @a buffer at @a offset in the file. See CFileDataIO::Write.
	 */
	void WriteAt(const void *buffer, uint64 offset, size_t count);

	/**
	 * True when the file position is at or past the end of the file.
	 */
	bool Eof();

	/**
	 * The file descriptor associated with the file. This defeats the purpose of the class, so
	 * AutoClose is disabled once fd() is called. Required for FileArea's mmap. FileArea objects
	 * are currently short-lived enough for that not to matter, but that might change.
	 */
	int fd();

	/**
	 * Re-enables AutoClose after a previous fd() disabled it.
	 */
	void Unlock();

private:
	//! A CFileAutoClose is neither copyable nor assignable.
	//@{
	CFileAutoClose(const CFileAutoClose &);
	CFileAutoClose &operator=(const CFileAutoClose &);
	//@}

	/**
	 * Check whether the file was autoclosed, and reopen it if needed.
	 */
	void Reopen();

	//! The wrapped CFile.
	CFile m_file;

	//! The mode used to open it.
	CFile::OpenMode m_mode;

	//! Is it temporarily closed?
	bool m_autoClosed;

	//! Autoclosing is disabled if != 0
	uint16 m_locked;

	//! Size before it was closed.
	uint64 m_size;

	//! Last access time (s)
	uint64 m_lastAccess;
};

#endif // FILEAUTOCLOSE_H
// File_checked_for_headers
