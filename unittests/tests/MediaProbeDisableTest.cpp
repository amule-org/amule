// SPDX-License-Identifier: GPL-2.0-or-later
#include <muleunit/test.h>
#include <atomic>
#include <wx/ffile.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#ifdef __WXMSW__
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
#ifdef __WXMSW__
	// A process in a Job with UI limits cannot join the parent's nested Job.
	// Fail the real parent confinement step before the crash fixture can run.
	const HANDLE job = CreateJobObjectW(nullptr, nullptr);
	ASSERT_TRUE(job != nullptr);
	ASSERT_TRUE(AssignProcessToJobObject(job, GetCurrentProcess()) != FALSE);
	JOBOBJECT_BASIC_UI_RESTRICTIONS ui{ JOB_OBJECT_UILIMIT_DESKTOP };
	ASSERT_TRUE(SetInformationJobObject(job, JobObjectBasicUIRestrictions, &ui, sizeof(ui)) != FALSE);
	const char firstMode = 'c';
#else
	const char firstMode = 'r';
#endif
	// 'c' crashes after the header and normally produces a file verdict. Once
	// restrictions fail, subsequent fixtures must never be launched again.
	for (const char mode : { firstMode, 'c', 'c' }) {
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
#ifdef __WXMSW__
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
#ifdef __WXMSW__
	CloseHandle(job);
#endif
	wxRemoveFile(path);
}
