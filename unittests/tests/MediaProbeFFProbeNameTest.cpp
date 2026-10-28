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

#include <sys/stat.h>

#include <wx/ffile.h>
#include <wx/filefn.h>
#include <wx/filename.h>

#include "MediaProbe.h"

using namespace muleunit;

DECLARE_SIMPLE(MediaProbeFFProbeName)

// Local validity check for an ffprobe path: used by auto-detection and by the monolithic
// Preferences dialog (which hard-rejects a non-ffprobe pick). The ffprobe path is NOT settable over
// EC, so this is a usability check, not a security boundary. The name check keeps an interpreter
// such as /bin/sh from being spawned at all; the banner check weeds out a mis-named-but-present
// ffprobe.

// --- file-name gate (pure, every platform) ------------------------------------------------------

TEST(MediaProbeFFProbeName, AcceptsFfprobeNames)
{
	ASSERT_TRUE(MediaProbe::HasFFProbeFileName(wxT("ffprobe")));
	ASSERT_TRUE(MediaProbe::HasFFProbeFileName(wxT("/usr/bin/ffprobe")));
	ASSERT_TRUE(MediaProbe::HasFFProbeFileName(wxT("ffprobe.exe")));
	ASSERT_TRUE(MediaProbe::HasFFProbeFileName(wxT("/opt/ffmpeg/bin/ffprobe.exe")));
	// Case-insensitive.
	ASSERT_TRUE(MediaProbe::HasFFProbeFileName(wxT("/opt/FFprobe")));
	ASSERT_TRUE(MediaProbe::HasFFProbeFileName(wxT("FFPROBE.EXE")));
}

TEST(MediaProbeFFProbeName, RejectsOtherNames)
{
	ASSERT_FALSE(MediaProbe::HasFFProbeFileName(wxT("/bin/sh")));
	ASSERT_FALSE(MediaProbe::HasFFProbeFileName(wxT("/usr/bin/python3")));
	ASSERT_FALSE(MediaProbe::HasFFProbeFileName(wxT("ffprobe2")));
	ASSERT_FALSE(MediaProbe::HasFFProbeFileName(wxT("notffprobe")));
	ASSERT_FALSE(MediaProbe::HasFFProbeFileName(wxT("ffprobe.sh")));
	ASSERT_FALSE(MediaProbe::HasFFProbeFileName(wxEmptyString));
}

// --- full gate: name + banner (needs an executable, POSIX only) ----------------------------------

#ifndef __WXMSW__

namespace
{

wxString MakeTempDir()
{
	const wxString base = wxFileName::CreateTempFileName(wxT("amule-ffprobe-gate"));
	wxRemoveFile(base);
	wxFileName::Mkdir(base, 0700, wxPATH_MKDIR_FULL);
	return base;
}

// Write <dir>/<name> as an executable shell script with the given body.
wxString WriteScript(const wxString &dir, const wxString &name, const wxString &body)
{
	const wxString path = dir + wxFileName::GetPathSeparator() + name;
	wxFFile f(path, wxT("w"));
	f.Write(wxString(wxT("#!/bin/sh\n")) + body);
	f.Close();
	chmod(path.fn_str(), 0755);
	return path;
}

} // namespace

// A file named ffprobe that prints the banner is accepted.
TEST(MediaProbeFFProbeName, AcceptsAnFfprobeThatPrintsTheBanner)
{
	const wxString dir = MakeTempDir();
	const wxString path = WriteScript(dir, wxT("ffprobe"), wxT("echo 'ffprobe version 6.0-test'\n"));
	ASSERT_TRUE(MediaProbe::IsFFProbeBinary(path));
	wxRemoveFile(path);
	wxRmdir(dir);
}

// A file named ffprobe that does NOT print the banner is rejected (an impostor named ffprobe).
TEST(MediaProbeFFProbeName, RejectsAnFfprobeWithoutTheBanner)
{
	const wxString dir = MakeTempDir();
	const wxString path = WriteScript(dir, wxT("ffprobe"), wxT("echo 'not a real tool'\n"));
	ASSERT_FALSE(MediaProbe::IsFFProbeBinary(path));
	wxRemoveFile(path);
	wxRmdir(dir);
}

// A program that prints the banner but is NOT named ffprobe is rejected by the name gate, so it is
// never spawned -- this is the /bin/sh case from the report.
TEST(MediaProbeFFProbeName, RejectsABannerPrinterWithTheWrongName)
{
	const wxString dir = MakeTempDir();
	const wxString path = WriteScript(dir, wxT("notffprobe"), wxT("echo 'ffprobe version 6.0-test'\n"));
	ASSERT_FALSE(MediaProbe::IsFFProbeBinary(path));
	wxRemoveFile(path);
	wxRmdir(dir);
}

#endif // __WXMSW__
