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

#include "Flatpak.h"
#include "PlayerExecutable.h"
#include "TestScratchDir.h"
#include "XdgConfigDir.h"

#include <wx/file.h>
#include <wx/filefn.h>
#include <array>
#include <unistd.h>

using namespace muleunit;

DECLARE_SIMPLE(PlayerExecutable)

namespace
{
class ScratchEnvironment
{
public:
	ScratchEnvironment()
	: m_cwd(wxGetCwd())
	{
		for (auto &env : m_env) {
			env.present = wxGetEnv(env.name, &env.value);
		}
		root = m_scratch.WxPath();
		wxFileName::Mkdir(root + "/first", 0700);
		wxFileName::Mkdir(root + "/second", 0700);
		wxSetWorkingDirectory(root);
		wxSetEnv("PATH", root + "/first:" + root + "/second");
		wxUnsetEnv("FLATPAK_ID");
	}

	~ScratchEnvironment()
	{
		wxSetWorkingDirectory(m_cwd);
		for (const auto &env : m_env) {
			if (env.present) {
				wxSetEnv(env.name, env.value);
			} else {
				wxUnsetEnv(env.name);
			}
		}
	}

	wxString File(const wxString &name, int mode = 0700) const
	{
		const wxString path = root + "/" + name;
		wxFile file;
		file.Create(path, true, mode);
		file.Write("#!/bin/sh\nexit 0\n");
		file.Close();
		return path;
	}

	wxString root;

private:
	struct Env
	{
		const char *name;
		wxString value;
		bool present = false;
	};
	const TestScratchDir m_scratch{ "amule-player-test" };
	const wxString m_cwd;
	std::array<Env, 4> m_env{ { { "PATH", {} },
		{ "FLATPAK_ID", {} },
		{ "XDG_CONFIG_HOME", {} },
		{ "HOST_XDG_CONFIG_HOME", {} } } };
};
} // namespace

TEST(PlayerExecutable, EmptyProgramIsRejected)
{
	ScratchEnvironment env;
	ASSERT_TRUE(!IsPlayerAvailable(""));
}

TEST(PlayerExecutable, MissingBareProgram)
{
	ScratchEnvironment env;
	ASSERT_TRUE(!IsPlayerAvailable("player"));
}

TEST(PlayerExecutable, FirstPathDirectory)
{
	ScratchEnvironment env;
	env.File("first/player");
	ASSERT_TRUE(IsPlayerAvailable("player"));
}

TEST(PlayerExecutable, LaterPathDirectory)
{
	ScratchEnvironment env;
	env.File("second/player");
	ASSERT_TRUE(IsPlayerAvailable("player"));
}

TEST(PlayerExecutable, SkipsNonExecutableMatch)
{
	ScratchEnvironment env;
	env.File("first/player", 0600);
	env.File("second/player");
	ASSERT_TRUE(IsPlayerAvailable("player"));
}

TEST(PlayerExecutable, NonExecutableOnlyIsRejected)
{
	ScratchEnvironment env;
	env.File("first/player", 0600);
	ASSERT_TRUE(!IsPlayerAvailable("player"));
}

TEST(PlayerExecutable, DirectoryIsNotAPlayer)
{
	ScratchEnvironment env;
	wxFileName::Mkdir(env.root + "/first/player", 0700);
	ASSERT_TRUE(!IsPlayerAvailable("player"));
}

TEST(PlayerExecutable, EmptyPathUsesCurrentDirectory)
{
	ScratchEnvironment env;
	env.File("player");
	wxSetEnv("PATH", "");
	ASSERT_TRUE(IsPlayerAvailable("player"));
	ASSERT_TRUE(!IsPlayerAvailable("absent"));
}

TEST(PlayerExecutable, LeadingEmptyPathEntry)
{
	ScratchEnvironment env;
	env.File("player");
	wxSetEnv("PATH", ":" + env.root + "/first");
	ASSERT_TRUE(IsPlayerAvailable("player"));
}

TEST(PlayerExecutable, TrailingEmptyPathEntry)
{
	ScratchEnvironment env;
	env.File("player");
	wxSetEnv("PATH", env.root + "/first:");
	ASSERT_TRUE(IsPlayerAvailable("player"));
}

TEST(PlayerExecutable, InteriorEmptyPathEntry)
{
	ScratchEnvironment env;
	env.File("player");
	wxSetEnv("PATH", env.root + "/first::" + env.root + "/second");
	ASSERT_TRUE(IsPlayerAvailable("player"));
}

TEST(PlayerExecutable, RelativePathDirectory)
{
	ScratchEnvironment env;
	env.File("second/player");
	wxSetEnv("PATH", "first:second");
	ASSERT_TRUE(IsPlayerAvailable("player"));
}

TEST(PlayerExecutable, ExplicitPathsDoNotSearchPath)
{
	ScratchEnvironment env;
	const wxString absolute = env.File("first/player");
	ASSERT_TRUE(IsPlayerAvailable(absolute));
	ASSERT_TRUE(IsPlayerAvailable("first/player"));
	ASSERT_TRUE(!IsPlayerAvailable("./player"));
	ASSERT_TRUE(!IsPlayerAvailable(env.root + "/player"));
	const wxString blocked = env.File("second/player", 0600);
	ASSERT_TRUE(!IsPlayerAvailable(blocked));
}

TEST(PlayerExecutable, SymlinksFollowTheirTarget)
{
	ScratchEnvironment env;
	const wxString target = env.File("second/player");
	const wxString link = env.root + "/first/player";
	ASSERT_EQUALS(0, symlink(target.fn_str(), link.fn_str()));
	ASSERT_TRUE(IsPlayerAvailable(link));
	wxRemoveFile(target);
	ASSERT_TRUE(!IsPlayerAvailable(link));
}

TEST(PlayerExecutable, UnsetPathDefersToExecvp)
{
	ScratchEnvironment env;
	wxUnsetEnv("PATH");
	ASSERT_TRUE(IsPlayerAvailable("player"));
	ASSERT_TRUE(!IsPlayerAvailable("./player"));
	ASSERT_TRUE(!IsPlayerAvailable(""));
}

TEST(PlayerExecutable, FlatpakDetectionTracksPresence)
{
	ScratchEnvironment env;
	ASSERT_TRUE(!InFlatpak());
	wxSetEnv("FLATPAK_ID", "org.amule.test");
	ASSERT_TRUE(InFlatpak());
	wxSetEnv("FLATPAK_ID", "");
	ASSERT_TRUE(InFlatpak());
	wxUnsetEnv("FLATPAK_ID");
	ASSERT_TRUE(!InFlatpak());
}

TEST(PlayerExecutable, NativeConfigUsesDesktopEnvironment)
{
	ScratchEnvironment env;
	wxSetEnv("XDG_CONFIG_HOME", env.root + "/native");
	wxSetEnv("HOST_XDG_CONFIG_HOME", env.root + "/host");
	ASSERT_EQUALS(env.root + "/native", XdgConfigDir());
}

TEST(PlayerExecutable, FlatpakConfigUsesHostEnvironmentEvenWithEmptyId)
{
	ScratchEnvironment env;
	wxSetEnv("XDG_CONFIG_HOME", env.root + "/sandbox");
	wxSetEnv("HOST_XDG_CONFIG_HOME", env.root + "/host");
	wxSetEnv("FLATPAK_ID", "org.amule.test");
	ASSERT_EQUALS(env.root + "/host", XdgConfigDir());
	wxSetEnv("FLATPAK_ID", "");
	ASSERT_EQUALS(env.root + "/host", XdgConfigDir());
}

TEST(PlayerExecutable, MissingHostConfigDoesNotUseSandboxDirectory)
{
	ScratchEnvironment env;
	wxSetEnv("XDG_CONFIG_HOME", env.root + "/sandbox");
	wxSetEnv("FLATPAK_ID", "org.amule.test");
	wxUnsetEnv("HOST_XDG_CONFIG_HOME");
	ASSERT_EQUALS(wxGetUserHome() + "/.config", XdgConfigDir());
	wxSetEnv("HOST_XDG_CONFIG_HOME", "");
	ASSERT_EQUALS(wxGetUserHome() + "/.config", XdgConfigDir());
}
