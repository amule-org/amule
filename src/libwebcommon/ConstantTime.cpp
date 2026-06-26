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

#include "ConstantTime.h"

namespace webcommon
{

bool ConstantTimeEquals(const std::string &a, const std::string &b)
{
	if (a.size() != b.size())
		return false;
	unsigned char acc = 0;
	for (size_t i = 0; i < a.size(); ++i) {
		acc |= static_cast<unsigned char>(a[i] ^ b[i]);
	}
	return acc == 0;
}

bool ConstantTimeEquals(const wxString &a, const wxString &b)
{
	const wxScopedCharBuffer au = a.utf8_str();
	const wxScopedCharBuffer bu = b.utf8_str();
	return ConstantTimeEquals(std::string(au.data(), au.length()), std::string(bu.data(), bu.length()));
}

} // namespace webcommon
