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

#include "MediaProbe.h"

using namespace muleunit;

DECLARE_SIMPLE(MediaProbeParse)

// Every fixture below is ffprobe's REAL output, captured verbatim from
// ffprobe 8.1.1 run with MediaProbe::ProbeEntries() against a file generated
// for that case. Recording the output rather than the media keeps the test
// free of binary fixtures and of any ffmpeg dependency in CI, while still
// pinning the actual wire format -- including the parts that are easy to get
// wrong from reading the docs: the DISPOSITION: prefix, the mixed key case
// Matroska emits, and N/A for an absent duration.

namespace
{
// Captured from ffprobe 8.1.1 against a generated cover.mp3 fixture.
const wxChar *const k_cover_mp3[] = {
	wxT("[STREAM]"),
	wxT("codec_name=mp3"),
	wxT("codec_type=audio"),
	wxT("DISPOSITION:attached_pic=0"),
	wxT("[/STREAM]"),
	wxT("[STREAM]"),
	wxT("codec_name=mjpeg"),
	wxT("codec_type=video"),
	wxT("DISPOSITION:attached_pic=1"),
	wxT("[/STREAM]"),
	wxT("[FORMAT]"),
	wxT("duration=5.000000"),
	wxT("bit_rate=130462"),
	wxT("TAG:artist=Test Artist"),
	wxT("TAG:album=Test Album"),
	wxT("TAG:title=Test Title"),
	wxT("[/FORMAT]"),
};

// Captured from ffprobe 8.1.1 against a generated cover.flac fixture.
const wxChar *const k_cover_flac[] = {
	wxT("[STREAM]"),
	wxT("codec_name=flac"),
	wxT("codec_type=audio"),
	wxT("DISPOSITION:attached_pic=0"),
	wxT("[/STREAM]"),
	wxT("[STREAM]"),
	wxT("codec_name=mjpeg"),
	wxT("codec_type=video"),
	wxT("DISPOSITION:attached_pic=1"),
	wxT("[/STREAM]"),
	wxT("[FORMAT]"),
	wxT("duration=N/A"),
	wxT("bit_rate=N/A"),
	wxT("TAG:artist=TestArtist"),
	wxT("TAG:album=TestAlbum"),
	wxT("TAG:title=TestTitle"),
	wxT("[/FORMAT]"),
};

// Captured from ffprobe 8.1.1 against a generated cover.m4a fixture.
const wxChar *const k_cover_m4a[] = {
	wxT("[STREAM]"),
	wxT("codec_name=aac"),
	wxT("codec_type=audio"),
	wxT("DISPOSITION:attached_pic=0"),
	wxT("[/STREAM]"),
	wxT("[STREAM]"),
	wxT("codec_name=mjpeg"),
	wxT("codec_type=video"),
	wxT("DISPOSITION:attached_pic=1"),
	wxT("[/STREAM]"),
	wxT("[FORMAT]"),
	wxT("duration=0.022993"),
	wxT("bit_rate=611316"),
	wxT("TAG:title=TestTitle"),
	wxT("TAG:artist=TestArtist"),
	wxT("TAG:album=TestAlbum"),
	wxT("[/FORMAT]"),
};

// Captured from ffprobe 8.1.1 against a generated tags.ogg fixture.
const wxChar *const k_tags_ogg[] = {
	wxT("[STREAM]"),
	wxT("codec_name=vorbis"),
	wxT("codec_type=audio"),
	wxT("DISPOSITION:attached_pic=0"),
	wxT("TAG:artist=TestArtist"),
	wxT("TAG:album=TestAlbum"),
	wxT("TAG:title=TestTitle"),
	wxT("[/STREAM]"),
	wxT("[FORMAT]"),
	wxT("duration=3.001179"),
	wxT("bit_rate=37761"),
	wxT("[/FORMAT]"),
};

// Captured from ffprobe 8.1.1 against a generated tags.mka fixture.
const wxChar *const k_tags_mka[] = {
	wxT("[STREAM]"),
	wxT("codec_name=aac"),
	wxT("codec_type=audio"),
	wxT("DISPOSITION:attached_pic=0"),
	wxT("[/STREAM]"),
	wxT("[FORMAT]"),
	wxT("duration=3.023000"),
	wxT("bit_rate=131972"),
	wxT("TAG:title=TestTitle"),
	wxT("TAG:ALBUM=TestAlbum"),
	wxT("TAG:ARTIST=TestArtist"),
	wxT("[/FORMAT]"),
};

// Captured from ffprobe 8.1.1 against a generated video_cover.mp4 fixture.
const wxChar *const k_video_cover_mp4[] = {
	wxT("[STREAM]"),
	wxT("codec_name=h264"),
	wxT("codec_type=video"),
	wxT("DISPOSITION:attached_pic=0"),
	wxT("[/STREAM]"),
	wxT("[STREAM]"),
	wxT("codec_name=aac"),
	wxT("codec_type=audio"),
	wxT("DISPOSITION:attached_pic=0"),
	wxT("[/STREAM]"),
	wxT("[STREAM]"),
	wxT("codec_name=mjpeg"),
	wxT("codec_type=video"),
	wxT("DISPOSITION:attached_pic=1"),
	wxT("[/STREAM]"),
	wxT("[FORMAT]"),
	wxT("duration=0.040000"),
	wxT("bit_rate=841800"),
	wxT("[/FORMAT]"),
};

// Captured from ffprobe 8.1.1 against a generated multitrack.mkv fixture.
const wxChar *const k_multitrack_mkv[] = {
	wxT("[STREAM]"),
	wxT("codec_name=h264"),
	wxT("codec_type=video"),
	wxT("DISPOSITION:attached_pic=0"),
	wxT("[/STREAM]"),
	wxT("[STREAM]"),
	wxT("codec_name=aac"),
	wxT("codec_type=audio"),
	wxT("DISPOSITION:attached_pic=0"),
	wxT("TAG:title=Deutsch"),
	wxT("[/STREAM]"),
	wxT("[STREAM]"),
	wxT("codec_name=aac"),
	wxT("codec_type=audio"),
	wxT("DISPOSITION:attached_pic=0"),
	wxT("TAG:title=Espanol"),
	wxT("[/STREAM]"),
	wxT("[FORMAT]"),
	wxT("duration=2.023000"),
	wxT("bit_rate=183533"),
	wxT("[/FORMAT]"),
};

// Captured from ffprobe 8.1.1 against a generated raw.h264 fixture.
const wxChar *const k_raw_h264[] = {
	wxT("[STREAM]"),
	wxT("codec_name=h264"),
	wxT("codec_type=video"),
	wxT("DISPOSITION:attached_pic=0"),
	wxT("[/STREAM]"),
	wxT("[FORMAT]"),
	wxT("duration=N/A"),
	wxT("bit_rate=N/A"),
	wxT("[/FORMAT]"),
};
// Feed one fixture through the parser.
bool Parse(const wxChar *const *lines, size_t count, MediaInfo &out)
{
	wxArrayString arr;
	for (size_t i = 0; i < count; ++i) {
		arr.Add(lines[i]);
	}
	return MediaProbe::ParseProbeOutput(arr, out);
}

} // namespace

#define PARSE(fixture, info) Parse(fixture, sizeof(fixture) / sizeof(fixture[0]), info)

// --- Cover art must never be taken for the file's codec (issue #1075) ----
// ffprobe reports embedded artwork as a regular video stream, so for an audio
// file it is the ONLY video stream and always won. The codec goes out on the
// wire to every peer, so this published "mjpeg" for a tagged music library.

TEST(MediaProbeParse, Mp3WithCoverReportsAudioCodecNotTheArtwork)
{
	MediaInfo info;
	ASSERT_TRUE(PARSE(k_cover_mp3, info));
	ASSERT_EQUALS(wxString(wxT("mp3")), info.codec);
	ASSERT_EQUALS(static_cast<uint32>(5), info.length_seconds);
	ASSERT_TRUE(info.bitrate_kbps > 0);
}

TEST(MediaProbeParse, FlacWithPictureBlockReportsFlac)
{
	MediaInfo info;
	ASSERT_TRUE(PARSE(k_cover_flac, info));
	ASSERT_EQUALS(wxString(wxT("flac")), info.codec);
}

TEST(MediaProbeParse, M4aWithCovrAtomReportsAac)
{
	MediaInfo info;
	ASSERT_TRUE(PARSE(k_cover_m4a, info));
	ASSERT_EQUALS(wxString(wxT("aac")), info.codec);
}

TEST(MediaProbeParse, VideoWithCoverStillReportsTheVideoCodec)
{
	// The fix must not change the answer for video: the real track wins
	// whatever position the artwork holds in the stream list.
	MediaInfo info;
	ASSERT_TRUE(PARSE(k_video_cover_mp4, info));
	ASSERT_EQUALS(wxString(wxT("h264")), info.codec);
}

// --- Tag extraction, and where each container hides the tags (#1076) -----

TEST(MediaProbeParse, FormatLevelTagsAreExtracted)
{
	MediaInfo info;
	ASSERT_TRUE(PARSE(k_cover_mp3, info));
	ASSERT_EQUALS(wxString(wxT("Test Artist")), info.artist);
	ASSERT_EQUALS(wxString(wxT("Test Album")), info.album);
	ASSERT_EQUALS(wxString(wxT("Test Title")), info.title);
}

TEST(MediaProbeParse, OggKeepsItsTagsOnTheStreamNotTheFormat)
{
	// Vorbis comments belong to the logical stream, so a format_tags-only
	// request loses them entirely for Ogg and Opus.
	MediaInfo info;
	ASSERT_TRUE(PARSE(k_tags_ogg, info));
	ASSERT_EQUALS(wxString(wxT("TestArtist")), info.artist);
	ASSERT_EQUALS(wxString(wxT("TestAlbum")), info.album);
	ASSERT_EQUALS(wxString(wxT("TestTitle")), info.title);
}

TEST(MediaProbeParse, MatroskaMixedCaseKeysAreRead)
{
	// One file, two cases: TAG:title lower, TAG:ALBUM and TAG:ARTIST upper.
	// ffprobe matches the requested names case-insensitively but prints the
	// container's own case.
	MediaInfo info;
	ASSERT_TRUE(PARSE(k_tags_mka, info));
	ASSERT_EQUALS(wxString(wxT("TestArtist")), info.artist);
	ASSERT_EQUALS(wxString(wxT("TestAlbum")), info.album);
	ASSERT_EQUALS(wxString(wxT("TestTitle")), info.title);
}

TEST(MediaProbeParse, MultiTrackVideoNeverPublishesATrackLabelAsTheTitle)
{
	// The audio streams here are labelled "Deutsch" and "Espanol" -- track
	// names, not song metadata. Falling back to stream tags on a video file
	// would advertise one of them as the file's title to every peer.
	MediaInfo info;
	ASSERT_TRUE(PARSE(k_multitrack_mkv, info));
	ASSERT_EQUALS(wxString(wxT("h264")), info.codec);
	ASSERT_TRUE(info.title.IsEmpty());
	ASSERT_TRUE(info.artist.IsEmpty());
	ASSERT_TRUE(info.album.IsEmpty());
}

// --- A codec with no duration is still a successful probe (#1077) --------

TEST(MediaProbeParse, RawElementaryStreamYieldsCodecWithoutDuration)
{
	// duration=N/A and bit_rate=N/A: the probe succeeds on the codec alone.
	// Treating this as a failure is what made these files re-probe forever.
	MediaInfo info;
	ASSERT_TRUE(PARSE(k_raw_h264, info));
	ASSERT_EQUALS(wxString(wxT("h264")), info.codec);
	ASSERT_EQUALS(static_cast<uint32>(0), info.length_seconds);
	ASSERT_EQUALS(static_cast<uint32>(0), info.bitrate_kbps);
}

TEST(MediaProbeParse, NothingUsableIsAFailedProbe)
{
	const wxChar *const empty[] = {
		wxT("[FORMAT]"), wxT("duration=N/A"), wxT("bit_rate=N/A"), wxT("[/FORMAT]")
	};
	MediaInfo info;
	ASSERT_TRUE(!PARSE(empty, info));
}

TEST(MediaProbeParse, StreamWithNoDispositionLineIsStillConsidered)
{
	// An ffprobe too old to know stream_disposition omits the line rather
	// than failing (an unknown FIELD is ignored; only an unknown SECTION is
	// fatal). Such a stream must still be eligible for codec selection.
	const wxChar *const old[] = { wxT("[STREAM]"),
		wxT("codec_name=mp3"),
		wxT("codec_type=audio"),
		wxT("[/STREAM]"),
		wxT("[FORMAT]"),
		wxT("duration=10.000000"),
		wxT("[/FORMAT]") };
	MediaInfo info;
	ASSERT_TRUE(PARSE(old, info));
	ASSERT_EQUALS(wxString(wxT("mp3")), info.codec);
	ASSERT_EQUALS(static_cast<uint32>(10), info.length_seconds);
}

TEST(MediaProbeParse, TagValueContainingAnEqualsSignSurvives)
{
	const wxChar *const eq[] = { wxT("[STREAM]"),
		wxT("codec_name=mp3"),
		wxT("codec_type=audio"),
		wxT("DISPOSITION:attached_pic=0"),
		wxT("[/STREAM]"),
		wxT("[FORMAT]"),
		wxT("duration=1.000000"),
		wxT("TAG:title=a=b=c"),
		wxT("[/FORMAT]") };
	MediaInfo info;
	ASSERT_TRUE(PARSE(eq, info));
	ASSERT_EQUALS(wxString(wxT("a=b=c")), info.title);
}
