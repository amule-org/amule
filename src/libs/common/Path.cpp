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

#include "Path.h"
#include "StringFunctions.h" // Needed for filename2char()

#include <wx/file.h>
#if defined __WINDOWS__ || defined __IRIX__
#include <wx/ffile.h>
#endif
#include <wx/utils.h>
#include <wx/filename.h>
#include <algorithm> // Needed for std::min

#ifndef __WINDOWS__
#include <sys/stat.h> // Needed for ::stat in GetFileStat
#endif

// Windows paths are case-insensitive, so compare that way there.
// TODO: lowercasing m_filesystem in the constructor may be simpler.
#ifdef __WINDOWS__
#define PATHCMP(a, b) wxStricmp(a, b)
#define PATHNCMP(a, b, n) wxStrnicmp(a, b, n)
#else
#define PATHCMP(a, b) wxStrcmp(a, b)
#define PATHNCMP(a, b, n) wxStrncmp(a, b, n)
#endif

////////////////////////////////////////////////////////////
// Helper functions

/** Creates a deep copy of the string, avoiding its ref. counting. */
inline wxString DeepCopy(const wxString &str)
{
	return wxString(str.c_str(), str.Length());
}

static wxString Demangle(const wxCharBuffer &fn, const wxString &filename)
{
	wxString result = wxConvUTF8.cMB2WC(fn);

	// FIXME: Is this actually needed for osx/msw?
	if (!result) {
		// Demangle further only for UTF-8, C and POSIX locales. For anything
		// else the current locale is probably the best choice for printing.
		static wxFontEncoding enc = wxLocale::GetSystemEncoding();

		switch (enc) {
		// SYSTEM is needed for ANSI encodings such as
		// "POSIX" and "C", which are only 7bit.
		case wxFONTENCODING_SYSTEM:
		case wxFONTENCODING_UTF8:
			result = wxConvISO8859_1.cMB2WC(fn);
			break;

		default:
			// Nothing to do, the filename is probably Ok.
			result = DeepCopy(filename);
		}
	}

	return result;
}

/** Splits a full path into its path and filename component. */
inline void DoSplitPath(const wxString &strPath, wxString *path, wxString *name)
{
	bool hasExt = false;
	wxString ext, vol;

	wxString *pVol = (path ? &vol : NULL);
	wxString *pExt = (name ? &ext : NULL);

	wxFileName::SplitPath(strPath, pVol, path, name, pExt, &hasExt);

	if (hasExt && pExt) {
		*name += "." + ext;
	}

	if (path && vol.Length()) {
		*path = vol + wxFileName::GetVolumeSeparator() + *path;
	}
}

/** Removes invalid chars from a filename. */
static wxString DoCleanup(const wxString &filename, bool keepSpaces, bool isFAT32)
{
	wxString result;
	for (size_t i = 0; i < filename.Length(); i++) {
		const wxChar c = filename[i];

		switch (c) {
		case '/':
			continue;

		case '\"':
		case '*':
		case '<':
		case '>':
		case '?':
		case '|':
		case '\\':
		case ':':
			if (isFAT32) {
				continue;
			}

		/* fall through */
		default:
			if ((c == ' ') && !keepSpaces) {
				result += "%20";
			} else if (c >= 32) {
				// Many illegal for filenames in windows
				// below the 32th char (which is space).
				result += filename[i];
			}
		}
	}

	return result;
}

/** Does the actual work of adding a postfix ... */
static wxString DoAddPostfix(const wxString &src, const wxString &postfix)
{
	wxFileName fn(src);

	fn.SetName(fn.GetName() + postfix);

	return fn.GetFullPath();
}

/** Removes the last extension of a filename. */
static wxString DoRemoveExt(const wxString &path)
{
	// Using wxFilename which handles paths, etc.
	wxFileName tmp(path);
	tmp.ClearExt();

	return tmp.GetFullPath();
}

/** Readies a path for use with wxAccess.. */
static wxString DoCleanPath(const wxString &path)
{
#ifdef __WINDOWS__
	// stat fails on windows if there are trailing path-separators.
	wxString cleanPath = StripSeparators(path, wxString::trailing);

	// Root paths must end with a separator (X:\ rather than X:).
	// See comments in wxDirExists.
	if ((cleanPath.Length() == 2) && (cleanPath.Last() == ':')) {
		cleanPath += wxFileName::GetPathSeparator();
	}

	return cleanPath;
#else
	return path;
#endif
}

/**
 * The canonical form IsSameAs() reduces a path to before comparing it.
 *
 * Computed once per path rather than twice per comparison. Grouping by this
 * string is equivalent to comparing pairwise, which lets CSharedFileList answer
 * a browse without re-deriving the grouping per directory (issue #898).
 *
 * Only for paths with a separator; bare filenames use PATHCMP.
 */
static wxString NormalizedKey(const wxString &path)
{
	// Only relative paths need the cwd: Normalize ignores it for absolute ones.
	// Skipping wxGetCwd() there (nearly every call site) avoids the wxLogSysError
	// it emits on macOS bundles whose recorded CWD is gone.
	wxString cwd;
	if (!wxIsAbsolutePath(path)) {
		cwd = wxGetCwd();
	}

	// Normalize all but env variables, which break when the string is not
	// encodable with wxConvLibc. Explicit flags: wxPATH_NORM_ALL is gone in wx3.
	const int flags = wxPATH_NORM_DOTS | wxPATH_NORM_TILDE | wxPATH_NORM_CASE | wxPATH_NORM_ABSOLUTE |
			  wxPATH_NORM_LONG | wxPATH_NORM_SHORTCUT;

	// wxFileName does the hard part. Note a trailing separator makes a path
	// unequal to the same path without one.
	wxFileName fn(path);
	fn.Normalize(flags, cwd);
	return fn.GetFullPath();
}

/** Returns true if the two paths are equal. */
static bool IsSameAs(const wxString &a, const wxString &b)
{
	// Fast path for bare filenames. Search results arrive as bare FT_FILENAME
	// strings and CSearchFile::AddChild dedups on every duplicate, so without
	// this each one falls through to Normalize and triggers wxGetCwd().
	if (a.find_first_of(wxFileName::GetPathSeparators()) == wxString::npos &&
		b.find_first_of(wxFileName::GetPathSeparators()) == wxString::npos) {
		return PATHCMP(a.c_str(), b.c_str()) == 0;
	}

	// Identical strings normalise identically, so both calls below are known
	// to agree before either runs.
	if (a == b) {
		return true;
	}

	// An empty path equals only another empty path, already answered above.
	// Falling through would normalise "" against the process cwd, making an
	// unset path compare equal to whatever directory aMule sits in. A
	// CKnownFile from known.met has no directory until the share scan stamps one.
	if (a.empty() || b.empty()) {
		return false;
	}

	return NormalizedKey(a) == NormalizedKey(b);
}

////////////////////////////////////////////////////////////
// CPath implementation

CPath::CPath() {}

CPath::CPath(const wxString &filename)
{
	// Equivalent to the default constructor ...
	if (!filename) {
		return;
	}

	wxCharBuffer fn = filename2char(filename);
	if (fn.data()) {
		// Valid in the current locale, so it came from a system call or a
		// correctly configured system.
		m_filesystem = DeepCopy(filename);
		m_printable = Demangle(fn, filename);
	} else {
		// Invalid in the current locale: save as UTF-8 even on a non-unicode
		// system, preserving the original until the user fixes their setup.
#ifdef __WINDOWS__
		// Magic fails on Windows where we always work with wide char file names.
		m_filesystem = DeepCopy(filename);
		m_printable = m_filesystem;
#else
		fn = filename.utf8_str();
		m_filesystem = wxConvFileName->cMB2WC(fn);

		// There's no need to try to unmangle the filename here.
		m_printable = DeepCopy(filename);
#endif
	}

	wxASSERT(m_filesystem.Length());
	wxASSERT(m_printable.Length());
}

CPath::CPath(const CPath &other)
: m_printable(DeepCopy(other.m_printable))
, m_filesystem(DeepCopy(other.m_filesystem))
{
}

CPath CPath::FromUniv(const wxString &path)
{
	wxCharBuffer fn = path.mb_str(wxConvISO8859_1);
	return CPath(wxConvFileName->cMB2WC(fn));
}

wxString CPath::ToUniv(const CPath &path)
{
	// Saved as a raw bytestream so the on-disk filename can always be
	// recreated, as if read through wx.
	wxCharBuffer fn = path.m_filesystem.mb_str(*wxConvFileName);
	return wxConvISO8859_1.cMB2WC(fn);
}

CPath &CPath::operator=(const CPath &other)
{
	if (this != &other) {
		m_printable = DeepCopy(other.m_printable);
		m_filesystem = DeepCopy(other.m_filesystem);
	}

	return *this;
}

bool CPath::operator==(const CPath &other) const
{
	return ::IsSameAs(m_filesystem, other.m_filesystem);
}

bool CPath::operator!=(const CPath &other) const
{
	return !(*this == other);
}

bool CPath::operator<(const CPath &other) const
{
	return PATHCMP(m_filesystem.c_str(), other.m_filesystem.c_str()) < 0;
}

bool CPath::IsOk() const
{
	// Something is very wrong if one of the two is empty.
	return m_printable.Length() && m_filesystem.Length();
}

bool CPath::FileExists() const
{
	return wxFileName::FileExists(m_filesystem);
}

bool CPath::DirExists() const
{
	return wxFileName::DirExists(DoCleanPath(m_filesystem));
}

bool CPath::IsDir(EAccess mode) const
{
	wxString path = DoCleanPath(m_filesystem);
	if (!wxFileName::DirExists(path)) {
		return false;
	} else if ((mode & writable) && !wxIsWritable(path)) {
		return false;
	} else if ((mode & readable) && !wxIsReadable(path)) {
		return false;
	}

	return true;
}

bool CPath::IsFile(EAccess mode) const
{
	if (!wxFileName::FileExists(m_filesystem)) {
		return false;
	} else if ((mode & writable) && !wxIsWritable(m_filesystem)) {
		return false;
	} else if ((mode & readable) && !wxIsReadable(m_filesystem)) {
		return false;
	}

	return true;
}

wxString CPath::GetRaw() const
{
	// Copy as c-strings to ensure that the CPath objects can safely
	// be passed across threads (avoiding wxString ref. counting).
	return DeepCopy(m_filesystem);
}

wxString CPath::GetPrintable() const
{
	// Copy as c-strings to ensure that the CPath objects can safely
	// be passed across threads (avoiding wxString ref. counting).
	return DeepCopy(m_printable);
}

wxString CPath::GetExt() const
{
	return wxFileName(m_filesystem).GetExt();
}

CPath CPath::GetPath() const
{
	CPath path;
	::DoSplitPath(m_printable, &path.m_printable, NULL);
	::DoSplitPath(m_filesystem, &path.m_filesystem, NULL);

	return path;
}

CPath CPath::GetFullName() const
{
	CPath path;
	::DoSplitPath(m_printable, NULL, &path.m_printable);
	::DoSplitPath(m_filesystem, NULL, &path.m_filesystem);

	return path;
}

sint64 CPath::GetFileSize() const
{
	if (FileExists()) {
		wxFile f(m_filesystem);
		if (f.IsOpened()) {
			return f.Length();
		}
	}

	return wxInvalidOffset;
}

bool CPath::GetFileStat(time_t &mtime, sint64 &size) const
{
#ifdef __WINDOWS__
	// Windows keeps the three separate wx calls in their original order, and
	// that order is load-bearing: wxFileName::GetTimes() reports failure through
	// wxLogSysError(), so asking about a missing path logs a system error.
	// FileExists() is what kept it quiet.
	//
	// So no saving here. One GetFileAttributesEx() would work, but its FILETIME
	// must convert to exactly the time_t wxFileModificationTime() returns:
	// known.met matches on mtime, and a second's drift re-hashes every share.
	if (!FileExists()) {
		return false;
	}

	const time_t fileDate = CPath::GetModificationTime(*this);
	const sint64 fileSize = GetFileSize();
	if ((fileDate == (time_t)-1) || (fileSize == wxInvalidOffset)) {
		return false;
	}
	mtime = fileDate;
	size = fileSize;
	return true;
#else
	const wxCharBuffer path = m_filesystem.mb_str(wxConvFile);
	if (!path.data()) {
		return false;
	}

	struct stat st;
	if (::stat(path.data(), &st) != 0) {
		return false;
	}

	// Same predicate wxFileExists() applies, explicit because it is what makes
	// the single stat a drop-in: a directory or broken link must still fail.
	if (!S_ISREG(st.st_mode)) {
		return false;
	}

	mtime = st.st_mtime;
	size = (sint64)st.st_size;
	return true;
#endif
}

wxString CPath::GetDirKey() const
{
	// The reduction IsSameDir() performs before comparing: strip the trailing
	// separator, then canonicalise. Equal keys exactly when IsSameDir() calls
	// them the same directory, so a caller can group instead of comparing
	// every pair (issue #898).
	wxString stripped = m_filesystem;
	if (stripped.Length()) {
		stripped = StripSeparators(stripped, wxString::trailing);
	}

	// NormalizedKey() and not IsSameAs()'s bare-filename fast path: a bare name
	// is made absolute against the cwd here, as IsSameAs() does once the other
	// side has a separator. Keying every path alike keeps the grouping consistent.
	return NormalizedKey(stripped);
}

bool CPath::IsSameDir(const CPath &other) const
{
	wxString a = m_filesystem;
	wxString b = other.m_filesystem;

	// Guards the case where one path is empty and the other is the root dir.
	if (a.Length() && b.Length()) {
		a = StripSeparators(a, wxString::trailing);
		b = StripSeparators(b, wxString::trailing);
	}

	return ::IsSameAs(a, b);
}

CPath CPath::JoinPaths(const CPath &other) const
{
	if (!IsOk()) {
		return CPath(other);
	} else if (!other.IsOk()) {
		return CPath(*this);
	}

	CPath joinedPath;
	// DeepCopy shouldn't be needed, as JoinPaths results in the creation of a new string.
	joinedPath.m_printable = ::JoinPaths(m_printable, other.m_printable);
	joinedPath.m_filesystem = ::JoinPaths(m_filesystem, other.m_filesystem);

	return joinedPath;
}

CPath CPath::Cleanup(bool keepSpaces, bool isFAT32) const
{
	CPath result;
	result.m_printable = ::DoCleanup(m_printable, keepSpaces, isFAT32);
	result.m_filesystem = ::DoCleanup(m_filesystem, keepSpaces, isFAT32);

	return result;
}

CPath CPath::AddPostfix(const wxString &postfix) const
{
	wxASSERT(postfix.IsAscii());

	CPath result;
	result.m_printable = ::DoAddPostfix(m_printable, postfix);
	result.m_filesystem = ::DoAddPostfix(m_filesystem, postfix);

	return result;
}

CPath CPath::AppendExt(const wxString &ext) const
{
	wxASSERT(ext.IsAscii());

	// Though technically, and empty extension would simply
	// be another . at the end of the filename, we ignore them.
	if (ext.IsEmpty()) {
		return *this;
	}

	CPath result(*this);
	if (ext[0] == '.') {
		result.m_printable << ext;
		result.m_filesystem << ext;
	} else {
		result.m_printable << "." << ext;
		result.m_filesystem << "." << ext;
	}

	return result;
}

CPath CPath::RemoveExt() const
{
	CPath result;
	result.m_printable = DoRemoveExt(m_printable);
	result.m_filesystem = DoRemoveExt(m_filesystem);

	return result;
}

CPath CPath::RemoveAllExt() const
{
	// Loop until all extensions are removed.
	//
	// Compares the filesystem strings directly rather than CPath::operator!=,
	// which routes through wxFileName::Normalize() and so wxGetCwd(). When the
	// recorded working directory has been deleted (routine on macOS bundles)
	// that logs an error on every call, and CDownloadListCtrl::DrawFileItem
	// reaches this loop on every paint of every row. Plain equality is also the
	// right test: we only need to know whether RemoveExt() changed the buffer.
	CPath last, current = RemoveExt();
	do {
		last = current;
		current = last.RemoveExt();
	} while (last.m_filesystem != current.m_filesystem);

	return current;
}

bool CPath::StartsWith(const CPath &other) const
{
	// Comparing invalid paths makes no sense: an empty 'other' would count as a
	// prefix of any path.
	if ((IsOk() && other.IsOk()) == false) {
		return false;
	}

	// Separator added to avoid partial matches, e.g. "/usr/bi" against
	// "/usr/bin". TODO: normalize paths in the constructor instead.
	const wxString a = StripSeparators(m_filesystem, wxString::trailing) + wxFileName::GetPathSeparator();
	const wxString b =
		StripSeparators(other.m_filesystem, wxString::trailing) + wxFileName::GetPathSeparator();

	if (a.Length() < b.Length()) {
		// Cannot possibly be a prefix.
		return false;
	}

	const size_t checkLen = std::min(a.Length(), b.Length());
	return PATHNCMP(a.c_str(), b.c_str(), checkLen) == 0;
}

bool CPath::RemoveFile(const CPath &file)
{
	return ::wxRemoveFile(file.m_filesystem);
}

bool CPath::RenameFile(const CPath &src, const CPath &dst, bool overwrite)
{
	return ::wxRenameFile(src.m_filesystem, dst.m_filesystem, overwrite);
}

bool CPath::BackupFile(const CPath &src, const wxString &appendix)
{
	wxASSERT(appendix.IsAscii());

	CPath dst = CPath(src.m_filesystem + appendix);

	// Small same-directory .met/config backup, so wxCopyFile's 4 KiB buffer is
	// fine. Large copies use CFile::CloneFile; mulecommon deliberately has no
	// dependency on the CFile layer (amule-org/amule#11).
	if (::wxCopyFile(src.m_filesystem, dst.m_filesystem, true)) {
		// Try to ensure that the backup gets physically written
#if defined __WINDOWS__ || defined __IRIX__
		wxFFile backupFile;
#else
		wxFile backupFile;
#endif
		if (backupFile.Open(dst.m_filesystem)) {
			backupFile.Flush();
		}

		return true;
	}

	return false;
}

bool CPath::RemoveDir(const CPath &file)
{
	return ::wxRmdir(file.m_filesystem);
}

bool CPath::MakeDir(const CPath &file)
{
	return ::wxMkdir(file.m_filesystem);
}

bool CPath::FileExists(const wxString &file)
{
	return CPath(file).FileExists();
}

bool CPath::DirExists(const wxString &path)
{
	return CPath(path).DirExists();
}

sint64 CPath::GetFileSize(const wxString &file)
{
	return CPath(file).GetFileSize();
}

time_t CPath::GetModificationTime(const CPath &file)
{
	return ::wxFileModificationTime(file.m_filesystem);
}

sint64 CPath::GetFreeSpaceAt(const CPath &path)
{
	wxLongLong free;
	if (::wxGetDiskSpace(path.m_filesystem, NULL, &free)) {
		return free.GetValue();
	}

	return wxInvalidOffset;
}

wxString CPath::TruncatePath(size_t length, bool isFilePath) const
{
	wxString file = GetPrintable();

	// Check if there's anything to do
	if (file.Length() <= length) {
		return file;
	}

	// If the path is a file name, then prefer to remove from the path, rather than the filename
	if (isFilePath) {
		wxString path = wxFileName(file).GetPath();
		file = wxFileName(file).GetFullName();

		if (path.Length() >= length) {
			path.Clear();
		} else if (file.Length() >= length) {
			path.Clear();
		} else {
			// Minus 6 for "[...]" + separator
			int pathlen = (int)(length - file.Length() - 6);

			if (pathlen > 0) {
				path = "[...]" + path.Right(pathlen);
			} else {
				path.Clear();
			}
		}

		file = ::JoinPaths(path, file);
	}

	if (file.Length() > length) {
		if (length > 5) {
			file = file.Left(length - 5) + "[...]";
		} else {
			file.Clear();
		}
	}

	return file;
}

wxString StripSeparators(wxString path, wxString::stripType type)
{
	wxASSERT((type == wxString::leading) || (type == wxString::trailing));
	const wxString seps = wxFileName::GetPathSeparators();

	while (!path.IsEmpty()) {
		size_t pos = ((type == wxString::leading) ? 0 : path.Length() - 1);

		if (seps.Contains(path.GetChar(pos))) {
			path.Remove(pos, 1);
		} else {
			break;
		}
	}

	return path;
}

wxString JoinPaths(const wxString &path, const wxString &file)
{
	if (path.IsEmpty()) {
		return file;
	} else if (file.IsEmpty()) {
		return path;
	}

	return StripSeparators(path, wxString::trailing) + wxFileName::GetPathSeparator() +
	       StripSeparators(file, wxString::leading);
}
