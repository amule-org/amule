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

#include <muleunit/test.h>

#include "PlayerExecutable.h"
#include "TestScratchDir.h"

#include <wx/ffile.h>
#include <wx/filefn.h>
#include <wx/filename.h>

using namespace muleunit;

DECLARE_SIMPLE(AppBundle)

TEST(AppBundle, OnlyAnExistingAppFolder)
{
	const TestScratchDir scratch("amule-bundle-");
	const wxString dir = scratch.WxPath();
	const wxString app = wxFileName(dir, "Player.APP").GetFullPath();
	const wxString file = wxFileName(dir, "Fake.app").GetFullPath();
	const wxString folder = wxFileName(dir, "Folder").GetFullPath();
	ASSERT_TRUE(wxMkdir(app));
	ASSERT_TRUE(wxMkdir(folder));
	ASSERT_TRUE(wxFFile(file, "w").Close());

	const bool bundle = IsAppBundle(app);
	const bool trailingSlash = IsAppBundle(app + "/");
	const bool plainFile = IsAppBundle(file);
	const bool plainFolder = IsAppBundle(folder);
	const bool missing = IsAppBundle(wxFileName(dir, "Missing.app").GetFullPath());

	ASSERT_TRUE(bundle);
	ASSERT_TRUE(trailingSlash);
	ASSERT_FALSE(plainFile);
	ASSERT_FALSE(plainFolder);
	ASSERT_FALSE(missing);
	ASSERT_FALSE(IsAppBundle(""));
}
