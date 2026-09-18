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

#include <atomic>

#include <wx/ffile.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/utils.h>

#include "MediaProbe.h"
#include "libs/common/Path.h"

using namespace muleunit;

DECLARE_SIMPLE(MediaProbeEnv)

// ffprobe is a host program, so it must not inherit an AppImage's library paths (#1463). The stand-
// in below reports the LD_LIBRARY_PATH it was started with as a title tag, which is what lets the
// test read the child's environment back through the normal parse path.
#ifndef __WXMSW__

namespace
{

const wxString kBundle = wxT("/tmp/amule-test-appdir");
const wxString kBundleLib = kBundle + wxT("/usr/lib");
const wxString kHostLib = wxT("/usr/lib");

// A shell script standing in for ffprobe: it ignores its arguments and prints two -of flat lines,
// one of them carrying its own LD_LIBRARY_PATH.
wxString WriteFakeFfprobe()
{
	const wxString path = wxFileName::CreateTempFileName(wxT("amule-fake-ffprobe"));
	wxFFile f(path, wxT("w"));
	const wxString script = wxString(wxT("#!/bin/sh\n")) +
				wxT("printf 'format.duration=\"1.000000\"\\n'\n") +
				wxT("printf 'format.tags.title=\"%s\"\\n' \"$LD_LIBRARY_PATH\"\n");
	f.Write(script);
	f.Close();
	wxExecute(wxT("chmod +x ") + path, wxEXEC_SYNC);
	return path;
}

// Probe() rejects a file it cannot stat, so the "media" has to exist.
wxString WriteDummyMedia()
{
	const wxString path = wxFileName::CreateTempFileName(wxT("amule-fake-media"));
	wxFFile f(path, wxT("w"));
	f.Write(wxT("x"));
	f.Close();
	return path;
}

wxString ProbeTitleWith(const wxString &appdir, const wxString &ldPath)
{
	const wxString ffprobe = WriteFakeFfprobe();
	const wxString media = WriteDummyMedia();

	if (appdir.IsEmpty()) {
		wxUnsetEnv(wxT("APPDIR"));
	} else {
		wxSetEnv(wxT("APPDIR"), appdir);
	}
	wxSetEnv(wxT("LD_LIBRARY_PATH"), ldPath);

	MediaInfo info;
	const std::atomic<bool> keepRunning(true);
	MediaProbe::Probe(ffprobe, CPath(media), info, 10000, keepRunning, false, false);

	wxUnsetEnv(wxT("APPDIR"));
	wxUnsetEnv(wxT("LD_LIBRARY_PATH"));
	wxRemoveFile(ffprobe);
	wxRemoveFile(media);
	return info.title;
}

} // namespace

// Inside a bundle, the child keeps the host entries and loses the bundle one.
TEST(MediaProbeEnv, BundlePathsAreStrippedFromTheChildEnvironment)
{
	ASSERT_EQUALS(kHostLib, ProbeTitleWith(kBundle, kBundleLib + wxT(":") + kHostLib));
}

// The control: with no $APPDIR there is no bundle, so the environment passes through untouched.
TEST(MediaProbeEnv, WithoutAppdirTheEnvironmentIsUnchanged)
{
	const wxString ldPath = kBundleLib + wxT(":") + kHostLib;
	ASSERT_EQUALS(ldPath, ProbeTitleWith(wxEmptyString, ldPath));
}

#endif // __WXMSW__
