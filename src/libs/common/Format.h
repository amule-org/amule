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

#ifndef FORMAT_H
#define FORMAT_H

#include <list>
#include <string> // Needed to use std::string
#include "MuleDebug.h"

/**
 * A typesafe alternative to wxString::Format, implemented against the printf description in
 * "man 3 printf".
 *
 * %CFormat lacks:
 *  - the @c "*" width-modifier, because only one argument is fed at a time,
 *  - the @c "n" type, which is unsafe and will not be implemented,
 *  - the @c "C" and @c "S" types, considered obsolete,
 *  - the Long Double type, which is extremely slow and should not be used.
 *
 * Support for the C99 @c a, @c A conversions and the non-standard @c ', @c I flags depends on the
 * underlying C library. Do not use them. The glibc-specific @c m conversion is supported on every
 * platform that offers an error description, and is thread-safe wherever that lookup is.
 *
 * Deviations from printf(3): %CFormat tries hard to format the passed POD type according to the
 * conversion type, converting basic types where needed, so a format accepts a variety of types:
 *  - @c c, @c i, @c d, @c u, @c o, @c x, @c X accept @c wxChar and all integer types,
 *  - @c a, @c A, @c e, @c E, @c f, @c F, @c g, @c G accept @c wxChar, integer and floating-point
 *    types,
 *  - @c p accepts only pointers,
 *  - @c s accepts all of the above plus @c wxString and @c wxChar*.
 *
 * The exception is integer (@c d, @c i, @c u) conversion, which always uses @c i or @c u according
 * to the signedness of the argument passed.
 *
 * @c 's' conversions follow the "we are converting to string anyway" mood, so each accepted type
 * gets a default conversion: @c 'c' for @c wxChar, @c 'i' and @c 'u' for signed and unsigned
 * integers, @c 'g' for floating-point numbers and @c 'p' for pointers.
 *
 * Other relaxations:
 *  - Length modifiers are read and validated, but always ignored, so an invalid combination of
 *    length modifier and conversion type is silently ignored (@c '%%qs' is treated as @c '%%s').
 *  - @c 'p' conversion ignores every modifier except the argument index reference.
 *  - Positional and indexed argument references can be mixed. (They actually cannot, because
 *    msgfmt treats that as an error.)
 *  - Indexed argument references may leave gaps in the indices.
 */
class CFormat
{
private:
	/**
	 * Holds a format specifier.
	 */
	struct FormatSpecifier
	{
		unsigned argIndex; //!< Argument index. (Position, unless specified otherwise.)
		wxChar flag;       //!< The optional flag character.
		unsigned width;    //!< The optional field width.
		signed precision;  //!< The optional precision value.
		// length is not stored
		wxChar type;     //!< The conversion type.
		size_t startPos; //!< Position of the first character of the format-specifier in the
				 //!< format-string.
		size_t endPos;   //!< Position of the last character of the format-specifier in the
				 //!< format-string.
		wxString result; //!< Result of the conversion. Initialized to the format-specifier.
	};

public:
	/**
	 * @param str The format string to be used.
	 */
	CFormat(const wxChar *str) { Init(str); }

	/**
	 * Required to construct from a plain char * with wx 2.9. @param str The format string to be
	 * used.
	 */
	CFormat(const wxString &str) { Init(str); }

	/**
	 * Feeds a value into the format string. A type incompatible with the current format field
	 * skips the field and raises an exception; any type fed to a CFormat with no free fields is
	 * ignored. Specialize this member template to teach CFormat other types.
	 */
	template <typename _Tp> CFormat &operator%(_Tp value);

	// Overload hack to map all pointer types to void*
	template <typename _Tp> CFormat &operator%(_Tp *value) { return this->operator% <void *>(value); }

	// explicit overloads to avoid pass-by-value even in debug builds.
	CFormat &operator%(const wxString &value) { return this->operator% <const wxString &>(value); }
	CFormat &operator%(const CFormat &value) { return this->operator% <const wxString &>(value); }
	CFormat &operator%(const std::string &value)
	{
		return this->operator% <const wxString &>(wxString(value.c_str(), wxConvUTF8));
	}
	// Narrow string literals are passed directly (after dropping the wxT()
	// wrapping sweep); dispatch to the wxString overload.
	CFormat &operator%(const char *value)
	{
		return this->operator% <const wxString &>(wxString(value, wxConvUTF8));
	}

	/**
	 * The resulting string.
	 */
	wxString GetString() const;

	/**
	 * Implicit conversion to wxString.
	 */
	operator wxString() const { return GetString(); };

private:
	/**
	 * Initializes member variables and parses the given format string.
	 */
	void Init(const wxString &str);

	//! Type holding format specifiers.
	typedef std::list<FormatSpecifier> FormatList;

	//! Retrieve the modifiers for the given format specifier.
	wxString GetModifiers(FormatList::const_iterator it) const;

	//! Do one argument conversion.
	template <typename _Tp> void ProcessArgument(FormatList::iterator it, _Tp value);

	//! List of the valid format-specifiers found in the format string.
	FormatList m_formats;

	//! Number of the previous argument.
	unsigned m_argIndex;

	//! The format-string fed to the parser.
	wxString m_formatString;
};

// type mappings
template <> inline CFormat &CFormat::operator%(char value)
{
	return *this % (wxChar)value;
}
template <> inline CFormat &CFormat::operator%(signed char value)
{
	return *this % (wxChar)value;
}
template <> inline CFormat &CFormat::operator%(unsigned char value)
{
	return *this % (wxChar)value;
}
template <> inline CFormat &CFormat::operator%(bool value)
{
	return *this % (signed long long)value;
}
template <> inline CFormat &CFormat::operator%(signed short value)
{
	return *this % (signed long long)value;
}
template <> inline CFormat &CFormat::operator%(unsigned short value)
{
	return *this % (unsigned long long)value;
}
template <> inline CFormat &CFormat::operator%(signed int value)
{
	return *this % (signed long long)value;
}
template <> inline CFormat &CFormat::operator%(unsigned int value)
{
	return *this % (unsigned long long)value;
}
template <> inline CFormat &CFormat::operator%(signed long value)
{
	return *this % (signed long long)value;
}
template <> inline CFormat &CFormat::operator%(unsigned long value)
{
	return *this % (unsigned long long)value;
}
template <> inline CFormat &CFormat::operator%(float value)
{
	return *this % (double)value;
}
template <> inline CFormat &CFormat::operator%(const wxChar *value)
{
	return this->operator% <const wxString &>(wxString(value));
}

#define WXLONGLONGFMTSPEC wxLongLongFmtSpec

#endif
// File_checked_for_headers
