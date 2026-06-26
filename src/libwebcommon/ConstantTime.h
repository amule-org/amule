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
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
//

#ifndef LIBWEBCOMMON_CONSTANTTIME_H
#define LIBWEBCOMMON_CONSTANTTIME_H

#include <string>

#include <wx/string.h>

// XOR-accumulator constant-time equality. Returns false immediately
// on length mismatch (length is not the secret); for equal-length
// inputs the timing is data-independent.
//
// **PRECONDITION on the `wxString` overload.** wxString is a UTF-16
// or UTF-32 sequence depending on the build; the comparator checks
// sequence length first, not byte length. Inputs that round-trip
// through different UTF-8 encodings and share an underlying string
// but differ in codepoint count will short-circuit as unequal.
// Callers today compare fixed-shape inputs (32-char hex MD5,
// 43-char base64url HMAC) so the precondition holds. Length-
// variable callers MUST pad to a common bound first or accept a
// length-side-channel leak.

namespace webcommon
{

bool ConstantTimeEquals(const std::string &a, const std::string &b);
bool ConstantTimeEquals(const wxString &a, const wxString &b);

} // namespace webcommon

#endif // LIBWEBCOMMON_CONSTANTTIME_H
