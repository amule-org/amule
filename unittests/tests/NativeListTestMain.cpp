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

#include "GuiTestDiagnostics.h"

#include "NativeListTest.h"

#include <wx/wx.h>
#include <wx/evtloop.h>
#include <iostream>
#include <stdexcept>
#include "Preferences.h"

// These preferences are the only application state read by the production
// renderers/list base linked into this standalone GUI test.
uint8 CPreferences::s_depth3D = 0;
bool CPreferences::s_liveListSort = false;

class LayoutTestApp : public wxApp
{
public:
	bool OnInit() override { return true; }
};
wxIMPLEMENT_APP_NO_MAIN(LayoutTestApp);

int main(int argc, char **argv)
{
	return RunGuiTest(
		argc,
		argv,
		"No GUI display available; skipping native list tests",
		[] {
			RunTests();
			std::cout << "Native list regression tests passed\n";
		},
		GuiTestEventLoop::Activate);
}
