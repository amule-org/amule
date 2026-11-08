// SPDX-License-Identifier: GPL-2.0-or-later
#include <muleunit/test.h>
#include <atomic>
#include <wx/ffile.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#if defined(__WXMSW__) && defined(AMULE_MEDIA_HELPER_TEST)
#include <windows.h>
#endif
#include "MediaProbe.h"
#include "libs/common/Path.h"

using namespace muleunit;
DECLARE_SIMPLE(MediaProbeDisable)

// A separate executable keeps a permanent session failure out of the other tests.
TEST(MediaProbeDisable, FailedRestrictionsDisableFurtherProbesWithoutFileVerdicts)
{
	const wxString path = wxFileName::CreateTempFileName(wxT("amule-restriction-failure"));
	const std::atomic<bool> running(true);
#if defined(__WXMSW__) && defined(AMULE_MEDIA_HELPER_TEST)
	// Prevent the production launcher's private desktop from being created.
	// Verify that the real restriction failure reaches the core unchanged.
	const HANDLE job = CreateJobObjectW(nullptr, nullptr);
	ASSERT_TRUE(job != nullptr);
	ASSERT_TRUE(AssignProcessToJobObject(job, GetCurrentProcess()) != FALSE);
	JOBOBJECT_BASIC_UI_RESTRICTIONS ui{ JOB_OBJECT_UILIMIT_DESKTOP };
	ASSERT_TRUE(SetInformationJobObject(job, JobObjectBasicUIRestrictions, &ui, sizeof(ui)) != FALSE);
#endif
	// 'c' crashes after the header and normally produces a file verdict. Once
	// restrictions fail, subsequent fixtures must never be launched again.
	for (const char mode : { 'r', 'c', 'c' }) {
		wxFFile input(path, wxT("wb"));
		ASSERT_TRUE(input.IsOpened());
		ASSERT_EQUALS(size_t(1), input.Write(&mode, 1));
		input.Close();
		MediaMetadata info;
		info.title = wxT("unchanged");
		wxString helper = wxString::FromUTF8(AMULE_MEDIA_EXIT_FIXTURE);
#if defined(__WXMSW__) && defined(AMULE_MEDIA_HELPER_TEST)
		if (mode == 'r')
			helper = wxString::FromUTF8(AMULE_MEDIA_HELPER_TEST);
#endif
		const auto outcome = MediaProbe::Probe(helper, CPath(path), info, 5000, running, true, false);
#if defined(__WXMSW__) && defined(AMULE_MEDIA_HELPER_TEST)
		// Remove the injected outer restriction. Subsequent probes must still
		// remain disabled, even though their confinement would now succeed.
		ui.UIRestrictionsClass = 0;
		ASSERT_TRUE(
			SetInformationJobObject(job, JobObjectBasicUIRestrictions, &ui, sizeof(ui)) != FALSE);
#endif
		ASSERT_TRUE(outcome == MediaProbe::ProbeOutcome::Unavailable);
		ASSERT_FALSE(MediaProbe::IsFileVerdict(outcome));
		ASSERT_EQUALS(wxString(wxT("unchanged")), info.title);
		ASSERT_TRUE(MediaProbe::DetectedPath(true).empty());
	}
#if defined(__WXMSW__) && defined(AMULE_MEDIA_HELPER_TEST)
	CloseHandle(job);
#endif
	wxRemoveFile(path);
}
