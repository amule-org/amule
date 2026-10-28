//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//
// Any parts of this program contributed by third-party developers are copyrighted
// by their respective authors.
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

#include "EncryptionRequirement.h"

using namespace muleunit;
using EncryptionRequirement::MustRefusePlainConnection;

DECLARE_SIMPLE(EncryptionRequirement)

TEST(EncryptionRequirement, ObfuscatingTransportSatisfiesRequire)
{
	ASSERT_FALSE(MustRefusePlainConnection(false, true, false));
	ASSERT_FALSE(MustRefusePlainConnection(true, true, false));
}

TEST(EncryptionRequirement, PlainTransportIsRefused)
{
	ASSERT_TRUE(MustRefusePlainConnection(false, false, false));
}

TEST(EncryptionRequirement, TestConnectionIsExemptUnlessStrict)
{
	ASSERT_FALSE(MustRefusePlainConnection(false, false, true));
	ASSERT_TRUE(MustRefusePlainConnection(true, false, true));
}
