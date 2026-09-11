//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 1998-2011 Vadim Zeitlin ( zeitlin@dptmaths.ens-cachan.fr )
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

#ifndef CFILE_H
#define CFILE_H

#include <common/Path.h> // Needed for CPath
#include "SafeFile.h"    // Needed for CFileDataIO

#include <wx/file.h> // Needed for constants

#include <memory> // Needed for std::unique_ptr
#include <mutex>  // Needed for std::recursive_mutex

#ifdef _MSC_VER // silly warnings about deprecated functions
#pragma warning(disable : 4996)
#endif

/**
 * A modified version of wxFile. Besides implementing the CFileDataIO interface, it
 * offers better support for UTF8 filenames and 64-bit file IO on both Windows and
 * unix-like systems.
 *
 * @see wxFile
 */
class CFile : public CFileDataIO
{
public:
	//! Standard values for file descriptor
	enum
	{
		fd_invalid = -1,
		fd_stdin,
		fd_stdout,
		fd_stderr
	};

	/** @see wxFile::OpenMode */
	enum OpenMode
	{
		read,
		write,
		read_write,
		write_append,
		write_excl,
		write_safe
	};

	/** Creates a closed file. */
	CFile();

	/**
	 * Calls Open on the specified file. Check IsOpened() to see whether it succeeded.
	 */
	CFile(const CPath &path, OpenMode mode = read);
	CFile(const wxString &path, OpenMode mode = read);

	/** Closes the file if opened. */
	virtual ~CFile();

	/**
	 * Opens a file.
	 *
	 * @param path The full or relative path to the file.
	 * @param mode The opening mode.
	 * @param accessMode The permissions in case a new file is created. Defaults to
	 * whatever CPreferences::GetFilePermissions gives.
	 * @return True if the file was opened.
	 *
	 * The 'write' and 'write_append' modes create the file if it does not exist.
	 * 'write_safe' appends ".new" to the file name and otherwise behaves like 'write',
	 * renaming to the original name on close -- Close() has to be called by hand, the
	 * destructor does not rename.
	 */
	bool Open(const CPath &path, OpenMode mode = read, int accessMode = wxS_DEFAULT);
	bool Open(const wxString &path, OpenMode mode = read, int accessMode = wxS_DEFAULT);

	/**
	 * Reopens a file which was opened and closed before, under the filename last
	 * opened. Throws on failure.
	 *
	 * @param mode The opening mode.
	 */
	void Reopen(OpenMode mode);

	/**
	 * Equivalent to Open with OpenMode 'write'.
	 *
	 * @param overwrite Whether an existing target file should be overwritten.
	 *
	 * @see CFile::Open
	 */
	bool Create(const CPath &path, bool overwrite = false, int accessMode = wxS_DEFAULT);
	bool Create(const wxString &path, bool overwrite = false, int accessMode = wxS_DEFAULT);

	/**
	 * Copies a file, streaming through a large buffer.
	 *
	 * Used for data-file copies that can cross filesystems (e.g. the Temp -> Incoming
	 * move on download completion, when a rename fails because the two directories live
	 * on different mounts). Unlike wxCopyFile's hard-coded 4 KiB buffer, this streams
	 * through a 1 MiB buffer, which is what lets cross-filesystem copies over NFS /
	 * sshfs reach line speed. See amule-org/amule#11.
	 *
	 * The silly name (vs. CopyFile) avoids the CopyFile #define set on MSW.
	 *
	 * @param overwrite If false and the destination exists, fails.
	 * @return true on success. On failure the partial destination is removed, so a
	 * failed copy never leaves a corrupt file behind.
	 */
	static bool CloneFile(const CPath &src, const CPath &dst, bool overwrite = false);

	/** Closes the file. Calling it on a closed file is illegal. */
	bool Close();

	/**
	 * Returns the file descriptor. Manipulating it directly should be avoided -- that
	 * is what this class is for.
	 */
	int fd() const;

	/** Flushes data not yet written. Calling it on a closed file is illegal. */
	bool Flush();

	/**
	 * @see CSafeFileIO::GetLength. Calling it on a closed file is illegal.
	 */
	virtual uint64 GetLength() const;

	/** Resizes the file to the specified length. */
	bool SetLength(uint64 newLength);

	/**
	 * @see CSafeFileIO::GetPosition. Calling it on a closed file is illegal.
	 */
	virtual uint64 GetPosition() const;

	/** Returns the bytes still available to read on the file before EOF. */
	virtual uint64 GetAvailable() const;

	/** Returns the path of the currently opened file. */
	const CPath &GetFilePath() const;

	/** Returns true if the file is opened. */
	bool IsOpened() const;

protected:
	/** @see CFileDataIO::doRead **/
	virtual sint64 doRead(void *buffer, size_t count) const;
	/** @see CFileDataIO::doWrite **/
	virtual sint64 doWrite(const void *buffer, size_t count);
	/** @see CFileDataIO::doSeek **/
	virtual sint64 doSeek(sint64 offset) const;

private:
	//! A CFile is neither copyable nor assignable.
	//@{
	CFile(const CFile &);
	CFile &operator=(const CFile &);
	//@}

	//! Flush any data in the userspace write buffer to the kernel. A no-op when the
	//! buffer is empty. Called from any path that needs the on-disk state to reflect
	//! preceding writes: Close, Flush, doSeek, doRead, GetPosition, GetLength.
	void DrainWriteBuffer() const;

	//! File descriptor or 'fd_invalid' if not opened
	int m_fd;

	//! The full path to the current file.
	CPath m_filePath;

	//! Are we using safe write mode?
	bool m_safeWrite;

	//! Userspace write buffer. CFile::doWrite was a thin wrapper over ::write(), so every
	//! CFileDataIO::WriteTag call and its small writes underneath round-tripped through
	//! the kernel: CKnownFileList::Save() with hundreds of thousands of files emitted
	//! ~26k write() syscalls per second and stalled shutdown for minutes (#562).
	//! Buffering into 64 KB chunks collapses millions of syscalls into a few thousand.
	//! The members are mutable so the const-qualified read / seek / position / length
	//! paths can drain transparently.
	//!
	//! Heap-allocated lazily rather than embedded as a fixed array, because CFile is
	//! regularly stack-allocated inside worker-thread call chains. musl's default pthread
	//! stack is 128 KiB, and two embedded 64 KiB buffers consume the whole stack, so the
	//! next stack-using call crosses the guard page and SIGSEGVs. The unique_ptr brings
	//! sizeof(CFile) back to ~32 bytes and pays the 64 KiB only on first write.
	enum
	{
		kWriteBufferSize = 64 * 1024
	};
	mutable std::unique_ptr<char[]> m_writeBuffer;
	mutable size_t m_writeBufferPending;

	//! True when the file was opened in a write-capable mode and buffering is safe.
	//! Read-only files bypass the buffer, so a stray write() through doWrite still fails
	//! immediately at the call site, preserving CFileDataIO's error contract.
	bool m_canBuffer;

	//! Serializes access to the userspace write buffer and fd state. Without it, paths
	//! that drain the buffer (Close, Flush, doSeek, doRead, GetPosition, GetLength,
	//! SetLength) race against concurrent doWrite/drain calls: two threads both pass the
	//! `pending != 0` check before either resets pending to 0, and since ::write advances
	//! the fd offset atomically per call, the same N bytes land at fd_pos AND fd_pos + N.
	//!
	//! Hit in production on CPartFile::m_hpartfile: FlushBuffer calls
	//! GetLength()/SetLength() and GetNeededSpace() calls GetLength(), all from the main
	//! thread without taking m_hpartfileMutex. Before #562 that was safe, since GetLength
	//! was just fstat; afterwards it drains the buffer, racing CPartFileWriteThread's
	//! FlushAt. Manifests as 1-3 corrupt blocks per part, with AICH recovering ~98%.
	//!
	//! Recursive so Open() -> Close() does not deadlock, and cheap against the ~1 us floor
	//! of the I/O syscalls it serializes. Mutable because the const-qualified methods need
	//! to lock it.
	mutable std::recursive_mutex m_mutex;
};

/** Thrown by CFile when a seek or tell fails. */
struct CSeekFailureException : public CIOFailureException
{
	CSeekFailureException(const wxString &desc);
};

#endif // CFILE_H
// File_checked_for_headers
