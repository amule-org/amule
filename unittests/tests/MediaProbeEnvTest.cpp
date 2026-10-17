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
#include <cstring>
#include <chrono>
#include <thread>
#include <vector>

#include <wx/ffile.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/utils.h>

#include "MediaProbe.h"
#include "libs/common/Path.h"

using namespace muleunit;

DECLARE_SIMPLE(MediaProbeEnv)

// The bundled helper inherits AppImage library paths. The stand-in returns
// LD_LIBRARY_PATH as a title so the test reads the actual child environment.
#ifndef __WXMSW__
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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
	const wxString script = wxT("#!/bin/sh\n") wxT(
		"printf 'AMMP "
		"1\\nformat.duration=1\\nformat.bit_rate=1000\\nstreams.stream.0.codec_type="
		"\"audio\"\\nstreams.stream.0.codec_name=\"mp3\"\\nformat.tags.artist=\"\"\\nformat.tags."
		"album=\"\"\\n'\n") wxT("printf 'format.tags.title=\"%s\"\\nEND\\n' \"$LD_LIBRARY_PATH\"\n");
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
	wxString originalAppdir, originalLdPath;
	const bool hadAppdir = wxGetEnv(wxT("APPDIR"), &originalAppdir);
	const bool hadLdPath = wxGetEnv(wxT("LD_LIBRARY_PATH"), &originalLdPath);

	if (appdir.IsEmpty()) {
		wxUnsetEnv(wxT("APPDIR"));
	} else {
		wxSetEnv(wxT("APPDIR"), appdir);
	}
	wxSetEnv(wxT("LD_LIBRARY_PATH"), ldPath);

	MediaInfo info;
	const std::atomic<bool> keepRunning(true);
	MediaProbe::Probe(ffprobe, CPath(media), info, 10000, keepRunning, false, false);

	if (hadAppdir)
		wxSetEnv(wxT("APPDIR"), originalAppdir);
	else
		wxUnsetEnv(wxT("APPDIR"));
	if (hadLdPath)
		wxSetEnv(wxT("LD_LIBRARY_PATH"), originalLdPath);
	else
		wxUnsetEnv(wxT("LD_LIBRARY_PATH"));
	wxRemoveFile(ffprobe);
	wxRemoveFile(media);
	return info.title;
}

} // namespace

// A bundled parser requires both bundle and host entries.
TEST(MediaProbeEnv, BundlePathsArePreservedForTheBundledHelper)
{
	ASSERT_EQUALS(
		kBundleLib + wxT(":") + kHostLib, ProbeTitleWith(kBundle, kBundleLib + wxT(":") + kHostLib));
}

// The control: with no $APPDIR there is no bundle, so the environment passes through untouched.
TEST(MediaProbeEnv, WithoutAppdirTheEnvironmentIsUnchanged)
{
	const wxString ldPath = kBundleLib + wxT(":") + kHostLib;
	ASSERT_EQUALS(ldPath, ProbeTitleWith(wxEmptyString, ldPath));
}

namespace
{
MediaInfo RunPolicyScript(const wxString &primary,
	unsigned timeout = 2000,
	MediaProbe::ProbeOutcome *outcome = nullptr,
	const std::atomic<bool> *keepRunning = nullptr)
{
	const wxString path = wxFileName::CreateTempFileName(wxT("amule-policy"));
	const wxString media = wxFileName::CreateTempFileName(wxT("amule-input"));
	wxFFile script(path, wxT("w"));
	script.Write(wxT("#!/bin/sh\n") + primary + wxT("\n"));
	script.Close();
	wxChmod(path, 0700);
	MediaInfo info;
	const std::atomic<bool> running(true);
	const auto result = MediaProbe::Probe(
		path, CPath(media), info, timeout, keepRunning ? *keepRunning : running, true, false);
	if (outcome)
		*outcome = result;
	wxRemoveFile(path);
	wxRemoveFile(media);
	return info;
}
wxString Reply(unsigned duration, const wxString &title)
{
	return wxString::Format(
		wxT("printf 'AMMP "
		    "1\\nformat.duration=%u\\nformat.bit_rate=1000\\nstreams.stream.0.codec_type="
		    "\"audio\"\\nstreams.stream.0.codec_name=\"mp3\"\\nformat.tags.artist=\"\"\\nformat.tags."
		    "album=\"\"\\nformat.tags.title=\"%s\"\\nEND\\n'"),
		duration,
		title);
}
} // namespace
TEST(MediaProbeEnv, CompleteResultIsPublished)
{
	ASSERT_EQUALS(wxString(wxT("primary")), RunPolicyScript(Reply(3, wxT("primary"))).title);
}
TEST(MediaProbeEnv, CodecOnlyResultIsPublished)
{
	ASSERT_EQUALS(wxString(wxT("primary")), RunPolicyScript(Reply(0, wxT("primary"))).title);
}
TEST(MediaProbeEnv, TimeoutIsCancelled)
{
	MediaProbe::ProbeOutcome outcome;
	RunPolicyScript(wxT("sleep 30"), 100, &outcome);
	ASSERT_TRUE(outcome == MediaProbe::ProbeOutcome::Cancelled);
}

TEST(MediaProbeEnv, HelperFailuresAreNeverPersistentFileVerdicts)
{
	for (const wxString &failure : { wxString(wxT("echo forged")),
		     wxString(wxT("exit 12")),
		     wxString(wxT("exit 127")),
		     wxString(wxT("exit 1")),
		     wxString(wxT("ulimit -c 0; kill -SEGV $$")) }) {
		MediaProbe::ProbeOutcome outcome;
		RunPolicyScript(failure, 2000, &outcome);
		ASSERT_TRUE(outcome == MediaProbe::ProbeOutcome::Unavailable);
		ASSERT_FALSE(MediaProbe::IsFileVerdict(outcome));
	}
}

TEST(MediaProbeEnv, RecognizedFileRejectionsRemainFileVerdicts)
{
	MediaProbe::ProbeOutcome outcome;
	RunPolicyScript(wxT("exit 10"), 2000, &outcome);
	ASSERT_TRUE(outcome == MediaProbe::ProbeOutcome::NoUsableMetadata);
	RunPolicyScript(wxT("exit 11"), 2000, &outcome);
	ASSERT_TRUE(outcome == MediaProbe::ProbeOutcome::UnreadableFile);
}

TEST(MediaProbeEnv, CancelledProbeDoesNotLaunchHelper)
{
	const wxString marker = wxFileName::CreateTempFileName(wxT("amule-cancel-marker"));
	wxRemoveFile(marker);
	const std::atomic<bool> running(false);
	MediaProbe::ProbeOutcome outcome;
	RunPolicyScript(
		wxT("touch '") + marker + wxT("'\n") + Reply(3, wxT("primary")), 2000, &outcome, &running);
	const bool launched = wxFileExists(marker);
	if (wxFileExists(marker))
		wxRemoveFile(marker);
	ASSERT_FALSE(launched);
	ASSERT_TRUE(outcome == MediaProbe::ProbeOutcome::Cancelled);
}

TEST(MediaProbeEnv, MalformedProtocolFieldsAreUnavailable)
{
	const wxString valid = Reply(3, wxT("primary"));
	for (const auto &change :
		std::vector<std::pair<wxString, wxString>>{ { wxT("AMMP 1"), wxT("AMMP 2") },
			{ wxT("duration=3"), wxT("duration=NaN") },
			{ wxT("codec_type=\"audio\""), wxT("codec_type=\"video\"") },
			{ wxT("title=\"primary\""), wxT("title=primary") },
			{ wxT("title=\"primary\""), wxT("title=\"broken\"quote\"") } }) {
		wxString malformed = valid;
		ASSERT_TRUE(malformed.Replace(change.first, change.second) > 0);
		MediaProbe::ProbeOutcome outcome;
		RunPolicyScript(malformed, 2000, &outcome);
		ASSERT_TRUE(outcome == MediaProbe::ProbeOutcome::Unavailable);
	}
}

TEST(MediaProbeEnv, MalformedProtocolFramingAndRangesAreUnavailable)
{
	const wxString valid = Reply(3, wxT("primary"));
	std::vector<wxString> malformed = {
		wxT("printf '\\n'; ") + valid,
		valid + wxT("; printf '\\n'"),
	};
	for (const auto &change : std::vector<std::pair<wxString, wxString>>{
		     { wxT("duration=3"), wxT("duration=4294967296") },
		     { wxT("bit_rate=1000"), wxT("bit_rate=4294967296000") },
		     { wxT("bit_rate=1000"), wxT("bit_rate=1001") },
		     { wxT("\\nformat.duration"), wxT("\\n\\nformat.duration") },
		     { wxT("\\nformat.duration"), wxT("\\r\\nformat.duration") },
		     { wxT("END\\n"), wxT("END") },
		     { wxT("AMMP 1"), wxT("AMMP 1\\000") },
	     }) {
		wxString reply = valid;
		ASSERT_TRUE(reply.Replace(change.first, change.second) > 0);
		malformed.push_back(reply);
	}
	for (const wxString &reply : malformed) {
		MediaProbe::ProbeOutcome outcome;
		RunPolicyScript(reply, 2000, &outcome);
		ASSERT_TRUE(outcome == MediaProbe::ProbeOutcome::Unavailable);
	}
}

TEST(MediaProbeEnv, ProtocolBoundaryValuesAreAccepted)
{
	wxString reply = Reply(0xffffffffu, wxT("maximum"));
	reply.Replace(wxT("bit_rate=1000"), wxT("bit_rate=4294967295000"));
	MediaProbe::ProbeOutcome outcome;
	const auto info = RunPolicyScript(reply, 2000, &outcome);
	ASSERT_TRUE(outcome == MediaProbe::ProbeOutcome::Extracted);
	ASSERT_EQUALS(uint32(0xffffffffu), info.length_seconds);
	ASSERT_EQUALS(uint32(0xffffffffu), info.bitrate_kbps);
	ASSERT_FALSE(MediaProbe::IsFileVerdict(MediaProbe::ProbeOutcome::OutputTooLarge));
}

TEST(MediaProbeEnv, PipeInputCannotBlockBeforeTheWatchdog)
{
	const wxString path = wxFileName::CreateTempFileName(wxT("amule-pipe-input"));
	wxRemoveFile(path);
	ASSERT_EQUALS(0, mkfifo(path.fn_str(), 0600));
	// Let the old blocking open complete too, so a regression fails an assertion
	// instead of hanging the test indefinitely.
	std::thread writer([&path] {
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		const int fd = open(path.fn_str(), O_WRONLY | O_NONBLOCK);
		if (fd >= 0)
			close(fd);
	});
	MediaInfo info;
	const std::atomic<bool> running(true);
	const auto start = std::chrono::steady_clock::now();
	const auto outcome = MediaProbe::Probe(wxT("/bin/true"), CPath(path), info, 50, running, true, false);
	const auto elapsed = std::chrono::steady_clock::now() - start;
	writer.join();
	wxRemoveFile(path);
	ASSERT_TRUE(elapsed < std::chrono::milliseconds(250));
	ASSERT_FALSE(MediaProbe::IsFileVerdict(outcome));
	ASSERT_FALSE(outcome == MediaProbe::ProbeOutcome::Extracted);
}

TEST(MediaProbeEnv, ExcessiveHelperOutputIsNotAFileVerdict)
{
	MediaProbe::ProbeOutcome outcome;
	RunPolicyScript(wxT("head -c 70000 /dev/zero"), 2000, &outcome);
	ASSERT_TRUE(outcome == MediaProbe::ProbeOutcome::Unavailable);
	ASSERT_FALSE(MediaProbe::IsFileVerdict(outcome));
}

TEST(MediaProbeEnv, ExcessiveOutputStopsARunningHelper)
{
	MediaProbe::ProbeOutcome outcome;
	const auto start = std::chrono::steady_clock::now();
	RunPolicyScript(wxT("head -c 70000 /dev/zero; sleep 30"), 2000, &outcome);
	ASSERT_TRUE(outcome == MediaProbe::ProbeOutcome::Unavailable);
	ASSERT_TRUE(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1000));
}

TEST(MediaProbeEnv, CancellationDiscardsResult)
{
	std::atomic<bool> running(true);
	std::thread cancel([&running] {
		std::this_thread::sleep_for(std::chrono::milliseconds(150));
		running = false;
	});
	MediaProbe::ProbeOutcome outcome;
	const MediaInfo info =
		RunPolicyScript(wxT("sleep 30\n") + Reply(3, wxT("primary")), 2000, &outcome, &running);
	cancel.join();
	ASSERT_TRUE(outcome == MediaProbe::ProbeOutcome::Cancelled);
	ASSERT_TRUE(info.title.empty());
}

#endif // __WXMSW__

#ifdef AMULE_MEDIA_HELPER_TEST
TEST(MediaProbeEnv, RealHelperReadsInheritedSeekableFile)
{
	const wxString path = wxFileName::CreateTempFileName(wxT("amule wave-媒体-"));
	const unsigned dataBytes = 8000 * 2 * 3;
	std::vector<unsigned char> bytes(44 + dataBytes, 0);
	const auto le = [&](unsigned at, unsigned value, unsigned count) {
		for (unsigned i = 0; i < count; ++i)
			bytes[at + i] = (value >> (i * 8)) & 255;
	};
	std::memcpy(bytes.data(), "RIFF", 4);
	le(4, 36 + dataBytes, 4);
	std::memcpy(bytes.data() + 8, "WAVEfmt ", 8);
	le(16, 16, 4);
	le(20, 1, 2);
	le(22, 1, 2);
	le(24, 8000, 4);
	le(28, 16000, 4);
	le(32, 2, 2);
	le(34, 16, 2);
	std::memcpy(bytes.data() + 36, "data", 4);
	le(40, dataBytes, 4);
	wxFFile file(path, wxT("wb"));
	file.Write(bytes.data(), bytes.size());
	file.Close();
	MediaInfo info;
	const std::atomic<bool> running(true);
	const auto result = MediaProbe::Probe(
		wxString::FromUTF8(AMULE_MEDIA_HELPER_TEST), CPath(path), info, 5000, running, true, false);
	wxRemoveFile(path);
	ASSERT_TRUE(result == MediaProbe::ProbeOutcome::Extracted);
	ASSERT_EQUALS(uint32(3), info.length_seconds);
	ASSERT_EQUALS(wxString(wxT("pcm_s16le")), info.codec);
}
#endif
