//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
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
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
//

#include <muleunit/test.h>
#include "MagnetURI.h"
using namespace muleunit;
DECLARE_SIMPLE(MagnetImport)
namespace
{
const char *const ED2K_HASH = "d41d8cd98f00b204e9800998ecf8427e";
}

TEST(MagnetImport, EscapedNameDelimitersStayInName)
{
	const auto link = CMagnetED2KConverter(
		STRING("magnet:?dn=name%26xl%3D999%7Cfile.bin&xt=urn:ed2k:") + ED2K_HASH + "&xl=42")
				  .GetED2KLink();
	ASSERT_EQUALS(STRING("ed2k://|file|name%26xl%3D999%7Cfile.bin|42|") + ED2K_HASH + "|/", link);
}
TEST(MagnetImport, EncodedIdentityAndLengthAreDecoded)
{
	const CMagnetED2KConverter converter(
		STRING("magnet:?dn=file.bin&xt=urn%3Aed2k%3A") + ED2K_HASH + "&xl=%34%32");
	ASSERT_TRUE(converter.CanConvertToED2K());
	ASSERT_EQUALS(STRING("ed2k://|file|file.bin|42|") + ED2K_HASH + "|/", converter.GetED2KLink());
}
TEST(MagnetImport, RawNamePipeIsRejected)
{
	ASSERT_TRUE(CMagnetED2KConverter(STRING("magnet:?dn=bad|field&xt=urn:ed2k:") + ED2K_HASH + "&xl=42")
			    .GetED2KLink()
			    .empty());
}
