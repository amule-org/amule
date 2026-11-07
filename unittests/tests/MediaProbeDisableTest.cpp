// SPDX-License-Identifier: GPL-2.0-or-later
#include <muleunit/test.h>
#include <atomic>
#include <wx/ffile.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include "MediaProbe.h"
#include "libs/common/Path.h"

using namespace muleunit;
DECLARE_SIMPLE(MediaProbeDisable)

// A separate executable keeps a permanent session failure out of the other tests.
TEST(MediaProbeDisable, FailedRestrictionsDisableFurtherProbesWithoutFileVerdicts)
{
	const wxString path = wxFileName::CreateTempFileName(wxT("amule-restriction-failure"));
	const std::atomic<bool> running(true);
	// 'c' crashes after the header and normally produces a file verdict. After
	// 'r' reports unavailable restrictions, it must never be launched again.
	for (const char mode : { 'r', 'c', 'c' }) {
		wxFFile input(path, wxT("wb"));
		ASSERT_TRUE(input.IsOpened());
		ASSERT_EQUALS(size_t(1), input.Write(&mode, 1));
		input.Close();
		MediaMetadata info;
		info.title = wxT("unchanged");
		const auto outcome = MediaProbe::Probe(wxString::FromUTF8(AMULE_MEDIA_EXIT_FIXTURE),
			CPath(path),
			info,
			5000,
			running,
			true,
			false);
		ASSERT_TRUE(outcome == MediaProbe::ProbeOutcome::Unavailable);
		ASSERT_FALSE(MediaProbe::IsFileVerdict(outcome));
		ASSERT_EQUALS(wxString(wxT("unchanged")), info.title);
		ASSERT_TRUE(MediaProbe::DetectedPath(true).empty());
	}
	wxRemoveFile(path);
}
