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

#ifndef TEXTFILE_H
#define TEXTFILE_H

#include <wx/ffile.h>
#include <wx/string.h>
#include <wx/strconv.h>

class CPath;

/** Criteria used when reading an entire file to an array of strings. */
enum EReadTextFile
{
	/** Do not filter anything */
	txtReadAll = 0,
	/** Do not return empty lines. Can be combined with txtStripWhiteSpace */
	txtIgnoreEmptyLines = 1,
	/** Do not return lines starting with a '#' */
	txtIgnoreComments = 2,
	/** Strip whitespace from the beginning/end of lines. */
	txtStripWhitespace = 4,

	/** Default parameters for file reading. */
	txtReadDefault = txtIgnoreEmptyLines | txtIgnoreComments | txtStripWhitespace
};

/**
 * Text file class.
 *
 * A wrapper around wxFFile, letting a text file be read or written line by line, with transparent
 * and automatic EOL-style handling.
 *
 * Seeking is not possible, only sequential reading or writing. The maximum length of a line is also
 * fixed (see CTextFile::GetNextLine), which the uses of this class make harmless.
 */
class CTextFile
{
public:
	// Open modes. Note that these are mutually exclusive!
	enum EOpenMode
	{
		//! Opens the file for reading, if it exists.
		read,
		//! Opens the file for writing, overwriting old contents.
		write
	};

	/* Constructor. */
	CTextFile();
	/** Destructor. Closes the file if still open. */
	~CTextFile();

	/** Opens the specified file, returning true on success. */
	//\{
	bool Open(const wxString &path, EOpenMode mode);
	bool Open(const CPath &path, EOpenMode mode);
	//\}

	/** Returns true if the file is opened. */
	bool IsOpened() const;
	/** Returns true if GetNextLine has reached the end of the file. */
	bool Eof() const;
	/** Closes the file, returning true on success. */
	bool Close();

	/**
	 * Returns the next line of a readable file.
	 *
	 * @param conv The converter used to convert from multibyte to widechar.
	 *
	 * Returns an empty string at EOF, and for a closed or unreadable file -- but empty lines in
	 * the file also come back empty, so use Eof() rather than this to test for EOF.
	 */
	wxString GetNextLine(
		EReadTextFile flags = txtReadAll, const wxMBConv &conv = wxConvLibc, bool *result = NULL);

	/**
	 * Writes the line to a writable file, returning true on success.
	 *
	 * @param conv The converter used to convert from widechar to multibyte.
	 */
	bool WriteLine(const wxString &line, const wxMBConv &conv = wxConvLibc);

	/** Reads and returns the contents of a text-file, using the specified criteria and converter. */
	wxArrayString ReadLines(EReadTextFile flags = txtReadDefault, const wxMBConv &conv = wxConvLibc);

	/** Writes the lines to the file, using the given converter, returning true if no errors occurred. */
	bool WriteLines(const wxArrayString &lines, const wxMBConv &conv = wxConvLibc);

private:
	//! The actual file object.
	wxFFile m_file;
	//! The mode in with which the file was opened.
	EOpenMode m_mode;
};

#endif /* TEXTFILE_H */
