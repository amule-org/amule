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
// ffprobe 8.1.1 run with MediaProbe::ProbeEntries() and -of flat against a file generated
// for that case. Recording the output rather than the media keeps the test
// free of binary fixtures and of any ffmpeg dependency in CI, while still
// pinning the actual wire format -- including the parts that are easy to get
// wrong from reading the docs: the DISPOSITION: prefix, the mixed key case
// Matroska emits, and N/A for an absent duration.

namespace
{
// Captured from ffprobe 8.1.1 (-of flat) against a generated cover.mp3 fixture.
const wxChar *const k_cover_mp3[] = {
	wxT("streams.stream.0.codec_name=\"mp3\""),
	wxT("streams.stream.0.codec_type=\"audio\""),
	wxT("streams.stream.0.disposition.attached_pic=0"),
	wxT("streams.stream.1.codec_name=\"mjpeg\""),
	wxT("streams.stream.1.codec_type=\"video\""),
	wxT("streams.stream.1.disposition.attached_pic=1"),
	wxT("format.duration=\"5.000000\""),
	wxT("format.bit_rate=\"130462\""),
	wxT("format.tags.artist=\"Test Artist\""),
	wxT("format.tags.album=\"Test Album\""),
	wxT("format.tags.title=\"Test Title\""),
};

// Captured from ffprobe 8.1.1 (-of flat) against a generated cover.flac fixture.
const wxChar *const k_cover_flac[] = {
	wxT("streams.stream.0.codec_name=\"flac\""),
	wxT("streams.stream.0.codec_type=\"audio\""),
	wxT("streams.stream.0.disposition.attached_pic=0"),
	wxT("streams.stream.1.codec_name=\"mjpeg\""),
	wxT("streams.stream.1.codec_type=\"video\""),
	wxT("streams.stream.1.disposition.attached_pic=1"),
	wxT("format.duration=\"N/A\""),
	wxT("format.bit_rate=\"N/A\""),
	wxT("format.tags.artist=\"TestArtist\""),
	wxT("format.tags.album=\"TestAlbum\""),
	wxT("format.tags.title=\"TestTitle\""),
};

// Captured from ffprobe 8.1.1 (-of flat) against a generated cover.m4a fixture.
const wxChar *const k_cover_m4a[] = {
	wxT("streams.stream.0.codec_name=\"aac\""),
	wxT("streams.stream.0.codec_type=\"audio\""),
	wxT("streams.stream.0.disposition.attached_pic=0"),
	wxT("streams.stream.1.codec_name=\"mjpeg\""),
	wxT("streams.stream.1.codec_type=\"video\""),
	wxT("streams.stream.1.disposition.attached_pic=1"),
	wxT("format.duration=\"0.022993\""),
	wxT("format.bit_rate=\"611316\""),
	wxT("format.tags.title=\"TestTitle\""),
	wxT("format.tags.artist=\"TestArtist\""),
	wxT("format.tags.album=\"TestAlbum\""),
};

// Captured from ffprobe 8.1.1 (-of flat) against a generated tags.ogg fixture.
const wxChar *const k_tags_ogg[] = {
	wxT("streams.stream.0.codec_name=\"vorbis\""),
	wxT("streams.stream.0.codec_type=\"audio\""),
	wxT("streams.stream.0.disposition.attached_pic=0"),
	wxT("streams.stream.0.tags.artist=\"TestArtist\""),
	wxT("streams.stream.0.tags.album=\"TestAlbum\""),
	wxT("streams.stream.0.tags.title=\"TestTitle\""),
	wxT("format.duration=\"3.001179\""),
	wxT("format.bit_rate=\"37761\""),
};

// Captured from ffprobe 8.1.1 (-of flat) against a generated tags.mka fixture.
const wxChar *const k_tags_mka[] = {
	wxT("streams.stream.0.codec_name=\"aac\""),
	wxT("streams.stream.0.codec_type=\"audio\""),
	wxT("streams.stream.0.disposition.attached_pic=0"),
	wxT("format.duration=\"3.023000\""),
	wxT("format.bit_rate=\"131972\""),
	wxT("format.tags.title=\"TestTitle\""),
	wxT("format.tags.ALBUM=\"TestAlbum\""),
	wxT("format.tags.ARTIST=\"TestArtist\""),
};

// Captured from ffprobe 8.1.1 (-of flat) against a generated video_cover.mp4 fixture.
const wxChar *const k_video_cover_mp4[] = {
	wxT("streams.stream.0.codec_name=\"h264\""),
	wxT("streams.stream.0.codec_type=\"video\""),
	wxT("streams.stream.0.disposition.attached_pic=0"),
	wxT("streams.stream.1.codec_name=\"aac\""),
	wxT("streams.stream.1.codec_type=\"audio\""),
	wxT("streams.stream.1.disposition.attached_pic=0"),
	wxT("streams.stream.2.codec_name=\"mjpeg\""),
	wxT("streams.stream.2.codec_type=\"video\""),
	wxT("streams.stream.2.disposition.attached_pic=1"),
	wxT("format.duration=\"0.040000\""),
	wxT("format.bit_rate=\"841800\""),
};

// Captured from ffprobe 8.1.1 (-of flat) against a generated multitrack.mkv fixture.
const wxChar *const k_multitrack_mkv[] = {
	wxT("streams.stream.0.codec_name=\"h264\""),
	wxT("streams.stream.0.codec_type=\"video\""),
	wxT("streams.stream.0.disposition.attached_pic=0"),
	wxT("streams.stream.1.codec_name=\"aac\""),
	wxT("streams.stream.1.codec_type=\"audio\""),
	wxT("streams.stream.1.disposition.attached_pic=0"),
	wxT("streams.stream.1.tags.title=\"Deutsch\""),
	wxT("streams.stream.2.codec_name=\"aac\""),
	wxT("streams.stream.2.codec_type=\"audio\""),
	wxT("streams.stream.2.disposition.attached_pic=0"),
	wxT("streams.stream.2.tags.title=\"Espanol\""),
	wxT("format.duration=\"2.023000\""),
	wxT("format.bit_rate=\"183533\""),
};

// Captured from ffprobe 8.1.1 (-of flat) against a generated raw.h264 fixture.
const wxChar *const k_raw_h264[] = {
	wxT("streams.stream.0.codec_name=\"h264\""),
	wxT("streams.stream.0.codec_type=\"video\""),
	wxT("streams.stream.0.disposition.attached_pic=0"),
	wxT("format.duration=\"N/A\""),
	wxT("format.bit_rate=\"N/A\""),
};

// Captured from ffprobe 8.1.1 (-of flat) against a generated evil.mp3 fixture.
const wxChar *const k_evil_mp3[] = {
	wxT("streams.stream.0.codec_name=\"mp3\""),
	wxT("streams.stream.0.codec_type=\"audio\""),
	wxT("streams.stream.0.disposition.attached_pic=0"),
	wxT("format.duration=\"1.000000\""),
	wxT("format.bit_rate=\"69112\""),
	wxT("format.tags.title=\"Song\\nduration=99999999\\nbit_rate=999000000\""),
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

TEST(MediaProbeParse, ZeroDurationWithNothingElseIsAFailedProbe)
{
	// "duration=0.000000" parses, but a zero duration is not a duration. If
	// this reported success the caller would treat an all-empty MediaInfo as
	// authoritative and CLEAR every media tag the file had -- including a
	// preview inherited from the search result -- then re-probe it to the
	// same effect on every subsequent start.
	const wxChar *const zero[] = { wxT("streams.stream.0.codec_type=\"data\""),
		wxT("streams.stream.0.disposition.attached_pic=0"),
		wxT("format.duration=\"0.000000\"") };
	MediaInfo info;
	ASSERT_TRUE(!PARSE(zero, info));
}

TEST(MediaProbeParse, ZeroDurationWithACodecStillSucceeds)
{
	// The intended case: zero length is a legitimate value to clear TO when
	// the probe did identify the file.
	const wxChar *const zeroCodec[] = { wxT("streams.stream.0.codec_name=\"h264\""),
		wxT("streams.stream.0.codec_type=\"video\""),
		wxT("streams.stream.0.disposition.attached_pic=0"),
		wxT("format.duration=\"0.000000\"") };
	MediaInfo info;
	ASSERT_TRUE(PARSE(zeroCodec, info));
	ASSERT_EQUALS(wxString(wxT("h264")), info.codec);
	ASSERT_EQUALS(static_cast<uint32>(0), info.length_seconds);
}

TEST(MediaProbeParse, SingleLabelledTrackInANonOggContainerIsNotATitle)
{
	// A one-track .mka muxed with --track-name 0:Deutsch passes every
	// STRUCTURAL test -- one audio stream, no video, nothing at format level --
	// so there is no way to tell its label from a title. The fallback is
	// therefore scoped to the containers that genuinely need it (the Ogg
	// family, whose comments live on the stream); an AAC track's stream tag is
	// a label and stays unpublished.
	const wxChar *const labelled[] = { wxT("streams.stream.0.codec_name=\"aac\""),
		wxT("streams.stream.0.codec_type=\"audio\""),
		wxT("streams.stream.0.disposition.attached_pic=0"),
		wxT("streams.stream.0.tags.title=\"Deutsch\""),
		wxT("format.duration=\"60.000000\"") };
	MediaInfo info;
	ASSERT_TRUE(PARSE(labelled, info));
	ASSERT_EQUALS(wxString(wxT("aac")), info.codec);
	ASSERT_TRUE(info.title.IsEmpty());
}

TEST(MediaProbeParse, MultiTrackAudioNeverPublishesATrackLabelAsTheTitle)
{
	// The audio-only twin of the multi-track video case. A .mka or chained
	// .ogg with two labelled audio streams and no format-level tags has no
	// video stream, so a "not a video" test would let the first track's label
	// through as the file's title. The fallback exists for Ogg/Opus, where
	// the comments live on the SINGLE logical stream -- so the condition is
	// exactly one audio stream.
	const wxChar *const multiAudio[] = { wxT("streams.stream.0.codec_name=\"vorbis\""),
		wxT("streams.stream.0.codec_type=\"audio\""),
		wxT("streams.stream.0.disposition.attached_pic=0"),
		wxT("streams.stream.0.tags.title=\"Track One\""),
		wxT("streams.stream.1.codec_name=\"vorbis\""),
		wxT("streams.stream.1.codec_type=\"audio\""),
		wxT("streams.stream.1.disposition.attached_pic=0"),
		wxT("streams.stream.1.tags.title=\"Track Two\""),
		wxT("format.duration=\"120.000000\"") };
	MediaInfo info;
	ASSERT_TRUE(PARSE(multiAudio, info));
	ASSERT_EQUALS(wxString(wxT("vorbis")), info.codec);
	ASSERT_TRUE(info.title.IsEmpty());
}

TEST(MediaProbeParse, SingleAudioStreamStillGetsItsStreamTags)
{
	// The case the fallback exists for must keep working: one logical stream,
	// tags on it, nothing at format level.
	MediaInfo info;
	ASSERT_TRUE(PARSE(k_tags_ogg, info));
	ASSERT_EQUALS(wxString(wxT("TestTitle")), info.title);
}

TEST(MediaProbeParse, CoverArtDoesNotCountAsASecondAudioStream)
{
	// Artwork is a video stream, so it must not disturb the audio count --
	// otherwise a tagged single-track MP3 would lose its stream-tag fallback.
	const wxChar *const oneAudioPlusCover[] = { wxT("streams.stream.0.codec_name=\"vorbis\""),
		wxT("streams.stream.0.codec_type=\"audio\""),
		wxT("streams.stream.0.disposition.attached_pic=0"),
		wxT("streams.stream.0.tags.title=\"Song\""),
		wxT("streams.stream.1.codec_name=\"mjpeg\""),
		wxT("streams.stream.1.codec_type=\"video\""),
		wxT("streams.stream.1.disposition.attached_pic=1"),
		wxT("format.duration=\"200.000000\"") };
	MediaInfo info;
	ASSERT_TRUE(PARSE(oneAudioPlusCover, info));
	ASSERT_EQUALS(wxString(wxT("vorbis")), info.codec);
	ASSERT_EQUALS(wxString(wxT("Song")), info.title);
}

// --- A crafted file must not be able to forge what we publish -----------
// Requesting format_tags / stream_tags puts attacker-controlled text into
// ffprobe's output for the first time -- container tags are arbitrary UTF-8
// and may contain newlines. With the `default` writer, which does not escape
// values, each newline became another key=value line inside the tag's own
// section, and this parser is last-write-wins: a title of
// "Song\nduration=99999999" overwrote the real duration, and the forged value
// was attached as FT_MEDIA_LENGTH and published to every server and Kad node.
// `-of flat` escapes the value instead, and carries the section and stream
// index in the key so no value can forge a boundary either.

TEST(MediaProbeParse, CraftedTagCannotForgeDurationOrBitrate)
{
	// Real ffprobe output for an mp3 whose title tag is literally
	// "Song\nduration=99999999\nbit_rate=999000000".
	MediaInfo info;
	ASSERT_TRUE(PARSE(k_evil_mp3, info));
	// The real values, not the injected ones.
	ASSERT_EQUALS(static_cast<uint32>(1), info.length_seconds);
	ASSERT_EQUALS(static_cast<uint32>(69), info.bitrate_kbps);
	// And the payload lands where it belongs -- in the title -- with its
	// newlines stripped by the sanitiser, so it cannot impersonate several
	// fields anywhere downstream either.
	ASSERT_EQUALS(wxString(wxT("Songduration=99999999bit_rate=999000000")), info.title);
	ASSERT_TRUE(info.title.Find(wxT('\n')) == wxNOT_FOUND);
}

TEST(MediaProbeParse, CraftedTagCannotForgeASectionBoundary)
{
	// The same idea aimed at the structure rather than a value: a tag whose
	// text looks like another stream's key. With flat output the injected
	// text stays inside the quoted value, so it can neither start a stream
	// nor overwrite one.
	const wxChar *const forge[] = { wxT("streams.stream.0.codec_name=\"mp3\""),
		wxT("streams.stream.0.codec_type=\"audio\""),
		wxT("streams.stream.0.disposition.attached_pic=0"),
		wxT("format.tags.artist=\"x\\nstreams.stream.0.codec_name=\\\"h264\\\"\""),
		wxT("format.duration=\"7.000000\"") };
	MediaInfo info;
	ASSERT_TRUE(PARSE(forge, info));
	ASSERT_EQUALS(wxString(wxT("mp3")), info.codec);
	ASSERT_EQUALS(static_cast<uint32>(7), info.length_seconds);
}

TEST(MediaProbeParse, OversizedTagValueIsCapped)
{
	// A container tag is arbitrary attacker-chosen text of arbitrary length,
	// and it ends up in known.met, in the log, and in every offered-file
	// packet sent to every server and client. The only bound downstream is the
	// wire format's 0xFFFF truncation, which is a packet-integrity guard, not
	// a policy.
	wxString huge(wxT('A'), 5000);
	wxArrayString lines;
	lines.Add(wxT("streams.stream.0.codec_name=\"mp3\""));
	lines.Add(wxT("streams.stream.0.codec_type=\"audio\""));
	lines.Add(wxT("streams.stream.0.disposition.attached_pic=0"));
	lines.Add(wxT("format.duration=\"3.000000\""));
	lines.Add(wxT("format.tags.title=\"") + huge + wxT("\""));
	MediaInfo info;
	ASSERT_TRUE(MediaProbe::ParseProbeOutput(lines, info));
	ASSERT_TRUE(info.title.length() <= 256);
	ASSERT_TRUE(info.title.length() > 0);
}

TEST(MediaProbeParse, ControlCharactersAreStrippedFromTagValues)
{
	// The unflattened value still contains real newlines -- flat only stopped
	// the PARSER being confused by them. They reach the log line and, through
	// it, GET /api/v0/logs/amule, where one field could impersonate several.
	const wxChar *const ctrl[] = { wxT("streams.stream.0.codec_name=\"mp3\""),
		wxT("streams.stream.0.codec_type=\"audio\""),
		wxT("streams.stream.0.disposition.attached_pic=0"),
		wxT("format.duration=\"3.000000\""),
		wxT("format.tags.title=\"a\\nb\\rc\"") };
	MediaInfo info;
	ASSERT_TRUE(PARSE(ctrl, info));
	ASSERT_EQUALS(wxString(wxT("abc")), info.title);
}

TEST(MediaProbeParse, OrdinaryTagTextIsUntouched)
{
	// The sanitiser must not mangle real metadata: non-ASCII stays, internal
	// spaces stay, only the edges are trimmed.
	const wxChar *const ok[] = { wxT("streams.stream.0.codec_name=\"mp3\""),
		wxT("streams.stream.0.codec_type=\"audio\""),
		wxT("streams.stream.0.disposition.attached_pic=0"),
		wxT("format.duration=\"3.000000\""),
		wxT("format.tags.artist=\"Sigur R\u00f3s & Friends\"") };
	MediaInfo info;
	ASSERT_TRUE(PARSE(ok, info));
	ASSERT_EQUALS(wxString::FromUTF8("Sigur R\xc3\xb3s & Friends"), info.artist);
}

TEST(MediaProbeParse, FlatEscapesAreUnwrapped)
{
	const wxChar *const esc[] = { wxT("streams.stream.0.codec_name=\"mp3\""),
		wxT("streams.stream.0.codec_type=\"audio\""),
		wxT("streams.stream.0.disposition.attached_pic=0"),
		wxT("format.duration=\"3.000000\""),
		wxT("format.tags.title=\"a\\\"b\\\\c\\nd\"") };
	MediaInfo info;
	ASSERT_TRUE(PARSE(esc, info));
	// \" and \\ unwrap to the real characters; the \n unwraps too and is then
	// dropped by the sanitiser, which is why there is no newline here.
	ASSERT_EQUALS(wxString(wxT("a\"b\\cd")), info.title);
}

TEST(MediaProbeParse, NothingUsableIsAFailedProbe)
{
	const wxChar *const empty[] = { wxT("format.duration=\"N/A\""), wxT("format.bit_rate=\"N/A\"") };
	MediaInfo info;
	ASSERT_TRUE(!PARSE(empty, info));
}

TEST(MediaProbeParse, StreamWithNoDispositionLineIsStillConsidered)
{
	// An ffprobe too old to know stream_disposition omits the line rather
	// than failing (an unknown FIELD is ignored; only an unknown SECTION is
	// fatal). Such a stream must still be eligible for codec selection.
	const wxChar *const old[] = { wxT("streams.stream.0.codec_name=\"mp3\""),
		wxT("streams.stream.0.codec_type=\"audio\""),
		wxT("format.duration=\"10.000000\"") };
	MediaInfo info;
	ASSERT_TRUE(PARSE(old, info));
	ASSERT_EQUALS(wxString(wxT("mp3")), info.codec);
	ASSERT_EQUALS(static_cast<uint32>(10), info.length_seconds);
}

TEST(MediaProbeParse, OldFfprobeWithoutDispositionLosesCoverAndTagsTogether)
{
	// The compound old-ffprobe case, recorded rather than argued about: with
	// no DISPOSITION line the artwork is indistinguishable from a real video
	// track, so it wins the codec AND -- because a non-empty videoCodec means
	// the file is treated as a video -- suppresses the stream-tag fallback.
	// Such an ffprobe therefore degrades to exactly master's behaviour for
	// these files. Nothing can be done about it from this side; the test
	// exists so the degradation is a recorded property rather than a surprise.
	const wxChar *const noDisp[] = { wxT("streams.stream.0.codec_name=\"vorbis\""),
		wxT("streams.stream.0.codec_type=\"audio\""),
		wxT("streams.stream.0.tags.artist=\"A\""),
		wxT("streams.stream.1.codec_name=\"mjpeg\""),
		wxT("streams.stream.1.codec_type=\"video\""),
		wxT("format.duration=\"5.000000\"") };
	MediaInfo info;
	ASSERT_TRUE(PARSE(noDisp, info));
	ASSERT_EQUALS(wxString(wxT("mjpeg")), info.codec);
	ASSERT_TRUE(info.artist.IsEmpty());

	// The same file from an ffprobe that DOES report the disposition gets
	// both right -- which is the whole point of requesting it.
	const wxChar *const withDisp[] = { wxT("streams.stream.0.codec_name=\"vorbis\""),
		wxT("streams.stream.0.codec_type=\"audio\""),
		wxT("streams.stream.0.disposition.attached_pic=0"),
		wxT("streams.stream.0.tags.artist=\"A\""),
		wxT("streams.stream.1.codec_name=\"mjpeg\""),
		wxT("streams.stream.1.codec_type=\"video\""),
		wxT("streams.stream.1.disposition.attached_pic=1"),
		wxT("format.duration=\"5.000000\"") };
	MediaInfo info2;
	ASSERT_TRUE(PARSE(withDisp, info2));
	ASSERT_EQUALS(wxString(wxT("vorbis")), info2.codec);
	ASSERT_EQUALS(wxString(wxT("A")), info2.artist);
}

TEST(MediaProbeParse, TagValueContainingAnEqualsSignSurvives)
{
	const wxChar *const eq[] = { wxT("streams.stream.0.codec_name=\"mp3\""),
		wxT("streams.stream.0.codec_type=\"audio\""),
		wxT("streams.stream.0.disposition.attached_pic=0"),
		wxT("format.duration=\"1.000000\""),
		wxT("format.tags.title=\"a=b=c\"") };
	MediaInfo info;
	ASSERT_TRUE(PARSE(eq, info));
	ASSERT_EQUALS(wxString(wxT("a=b=c")), info.title);
}
