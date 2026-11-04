// SPDX-License-Identifier: GPL-2.0-or-later
// Isolated metadata-only parser, adapted from 3togo/mini-media-probe.
// The core owns the wall-clock watchdog and Windows Job object.
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <sys/stat.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <sys/resource.h>
#include <unistd.h>
static_assert(sizeof(off_t) >= sizeof(int64_t), "Media probe requires 64-bit file offsets");
#endif
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avstring.h>
#include <libavutil/dict.h>
#include <libavutil/mem.h>
#include <libavutil/version.h>
}

namespace
{
constexpr uint64_t MaxRead = 1024ULL * 1024 * 1024;
constexpr unsigned MaxBackwardSeeks = 256;
using Clock = std::chrono::steady_clock;
struct Input
{
	int64_t size = 0;
	uint64_t bytes = 0;
	unsigned backwardSeeks = 0;
	bool exhausted = false;
	bool ioError = false;
	Clock::time_point deadline = Clock::now() + std::chrono::seconds(25);
};
struct Result
{
	uint32_t length = 0, bitrate = 0;
	std::string codec, artist, album, title;
};
// Layer III frame duration is fixed by its MPEG version/sample rate, not bitrate.
// Count complete frames across gaps and ignore partial/trailing packets. The
// result is useful programme timing even when a few damaged frames are skipped.
// These header-size rules follow libavcodec/mpegaudiodecheader.c; its full
// validator is private API. Free-format frames are deliberately left unknown.
bool mp3FrameTicks(const AVPacket &packet, int64_t &ticks)
{
	if (packet.size < 4 || (packet.flags & AV_PKT_FLAG_CORRUPT))
		return false;
	const uint32_t header = uint32_t(packet.data[0]) << 24 | uint32_t(packet.data[1]) << 16 |
				uint32_t(packet.data[2]) << 8 | packet.data[3];
	const unsigned version = (header >> 19) & 3;
	const unsigned bitrateIndex = (header >> 12) & 15;
	const unsigned rateIndex = (header >> 10) & 3;
	if ((header & 0xffe00000U) != 0xffe00000U || version == 1 || ((header >> 17) & 3) != 1 ||
		!bitrateIndex || bitrateIndex == 15 || rateIndex == 3 || (header & 3) == 2)
		return false;
	constexpr unsigned rates[] = { 44100, 48000, 32000 };
	constexpr unsigned mpeg1[] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 };
	constexpr unsigned mpeg2[] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 };
	const unsigned rate = rates[rateIndex] / (version == 3 ? 1 : version == 2 ? 2 : 4);
	const unsigned bitrate = version == 3 ? mpeg1[bitrateIndex] : mpeg2[bitrateIndex];
	const unsigned bytes = (version == 3 ? 144000 : 72000) * bitrate / rate + ((header >> 9) & 1);
	if (static_cast<unsigned>(packet.size) != bytes)
		return false;
	// 14,112,000 is divisible by every Layer III sample rate; integer sums avoid drift.
	ticks = (version == 3 ? 1152LL : 576LL) * (14112000 / rate);
	return true;
}
bool scanMp3(AVFormatContext *format,
	const AVStream *stream,
	const Input &input,
	double &seconds,
	uint64_t &frameBytes)
{
	if (input.size > 512LL * 1024 * 1024)
		return false;
	auto *packet = av_packet_alloc();
	if (!packet)
		return false;
	struct Owner
	{
		AVPacket *&packet;
		~Owner() { av_packet_free(&packet); }
	} owner{ packet };
	int64_t total = 0;
	frameBytes = 0;
	int status = 0;
	// find_stream_info buffers the initial packets: read_frame starts at the
	// logical beginning without seeking away encoder-delay or cover-art state.
	while ((status = av_read_frame(format, packet)) >= 0) {
		if (packet->stream_index == stream->index) {
			int64_t ticks = 0;
			if (mp3FrameTicks(*packet, ticks) &&
				total <= std::numeric_limits<int64_t>::max() - ticks) {
				total += ticks;
				frameBytes += static_cast<unsigned>(packet->size);
			}
		}
		av_packet_unref(packet);
	}
	if (status != AVERROR_EOF || input.exhausted || Clock::now() >= input.deadline ||
		std::ferror(stdin) || !total)
		return false;
	seconds = double(total) / 14112000;
	return true;
}
int64_t position()
{
#ifdef _WIN32
	return _ftelli64(stdin);
#else
	return ftello(stdin);
#endif
}
int reposition(int64_t offset, int whence)
{
#ifdef _WIN32
	return _fseeki64(stdin, offset, whence);
#else
	return fseeko(stdin, offset, whence);
#endif
}
int interrupted(void *opaque)
{
	const auto &input = *static_cast<Input *>(opaque);
	return input.exhausted || Clock::now() >= input.deadline;
}
int readInput(void *opaque, uint8_t *buffer, int capacity)
{
	auto &input = *static_cast<Input *>(opaque);
	if (interrupted(opaque))
		return AVERROR_EXIT;
	if (input.bytes >= MaxRead) {
		input.exhausted = true;
		return AVERROR(EFBIG);
	}
	const auto count = std::fread(buffer, 1, std::min<uint64_t>(capacity, MaxRead - input.bytes), stdin);
	input.bytes += count;
	if (std::ferror(stdin))
		input.ioError = true;
	if (count)
		return static_cast<int>(count);
	if (input.ioError)
		return AVERROR(EIO);
	return AVERROR_EOF;
}
int64_t seekInput(void *opaque, int64_t offset, int whence)
{
	auto &input = *static_cast<Input *>(opaque);
	if (whence == AVSEEK_SIZE)
		return input.size;
	if (interrupted(opaque))
		return AVERROR_EXIT;
	whence &= ~AVSEEK_FORCE;
	const auto current = position();
	const auto base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? current : input.size;
	if (current < 0) {
		input.ioError = true;
		return AVERROR(EIO);
	}
	if ((whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) || offset < -base ||
		offset > input.size - base)
		return AVERROR(EINVAL);
	const auto target = base + offset;
	if (target <= current && ++input.backwardSeeks > MaxBackwardSeeks) {
		input.exhausted = true;
		return AVERROR(EFBIG);
	}
	if (reposition(target, SEEK_SET)) {
		input.ioError = true;
		return AVERROR(EIO);
	}
	return target;
}
int denyOpen(AVFormatContext *, AVIOContext **, const char *, int, AVDictionary **)
{
	// Containers must never open external tracks, playlists, URLs or local files.
	return AVERROR(EACCES);
}
std::string clean(const char *text)
{
	if (!text)
		return {};
	std::string result;
	const auto *cursor = reinterpret_cast<const uint8_t *>(text);
	const auto *end = cursor + std::strlen(text);
	bool utf8 = true;
	while (cursor < end) {
		int32_t codepoint = 0;
		if (av_utf8_decode(&codepoint, &cursor, end, 0) < 0) {
			utf8 = false;
			break;
		}
	}
	cursor = reinterpret_cast<const uint8_t *>(text);
	while (cursor < end && result.size() < 4096) {
		const auto *start = cursor;
		int32_t codepoint = 0;
		if (utf8) {
			if (av_utf8_decode(&codepoint, &cursor, end, 0) < 0)
				break;
		} else
			codepoint = *cursor++;
		if (codepoint < 32 && codepoint != '\t' && codepoint != '\n' && codepoint != '\r')
			continue;
		if (codepoint == 127 || (codepoint >= 128 && codepoint <= 159))
			continue;
		if (!utf8 && codepoint >= 128) {
			if (result.size() + 2 > 4096)
				break;
			result += static_cast<char>(0xc0 | (codepoint >> 6));
			result += static_cast<char>(0x80 | (codepoint & 0x3f));
			continue;
		}
		const auto length = static_cast<size_t>(cursor - start);
		if (result.size() + length > 4096)
			break;
		result.append(reinterpret_cast<const char *>(start), length);
	}
	return result;
}
std::string tag(const AVDictionary *dict, const char *key)
{
	const auto *entry = av_dict_get(dict, key, nullptr, 0);
	return entry ? clean(entry->value) : std::string();
}
uint32_t bounded(double value, bool round)
{
	if (!std::isfinite(value) || value <= 0)
		return 0;
	return static_cast<uint32_t>(std::min(round ? std::floor(value + 0.5) : std::floor(value),
		double(std::numeric_limits<uint32_t>::max())));
}
int probe(Input &input, Result &result)
{
	auto *buffer = static_cast<unsigned char *>(av_malloc(64 * 1024));
	if (!buffer)
		return 12;
	auto *io = avio_alloc_context(buffer, 64 * 1024, 0, &input, readInput, nullptr, seekInput);
	if (!io) {
		av_free(buffer);
		return 12;
	}
	auto *format = avformat_alloc_context();
	if (!format) {
		av_free(io->buffer);
		io->buffer = nullptr;
		avio_context_free(&io);
		return 12;
	}
	struct Owner
	{
		AVFormatContext *&format;
		AVIOContext *&io;
		~Owner()
		{
			avformat_close_input(&format);
			// libavformat can replace the original AVIO buffer.
			av_free(io->buffer);
			io->buffer = nullptr;
			avio_context_free(&io);
		}
	} owner{ format, io };
	format->pb = io;
	format->flags |= AVFMT_FLAG_CUSTOM_IO;
	format->interrupt_callback = { interrupted, &input };
	format->io_open = denyOpen;
	format->probesize = 5 * 1024 * 1024;
	format->max_analyze_duration = 5 * AV_TIME_BASE;
	AVDictionary *options = nullptr;
	// Fail closed if allocating any policy option fails. These restrictions must
	// also hold with full distro libraries, not just the reduced bundle.
	// Match packaging/media/build-ffmpeg.py, including runtime format aliases.
	const char *formats =
		"aac,ac3,aiff,amr,ape,asf,au,avi,caf,dts,dv,eac3,flac,flv,gsm,h264,hevc,ivf,m4v,"
		"matroska,webm,mov,mp4,m4a,3gp,3g2,mj2,mp3,mpc,mpc8,mpeg,mpegts,mpegvideo,mxf,"
		"ogg,rm,w64,wav,wtv,wv,tta";
	// find_stream_info must not open decoders supplied by system FFmpeg.
	if (av_dict_set(&options, "protocol_whitelist", "", 0) < 0 ||
		av_dict_set(&options, "format_whitelist", formats, 0) < 0 ||
		av_dict_set(&options, "codec_whitelist", "", 0) < 0) {
		av_dict_free(&options);
		return 12;
	}
	const int opened = avformat_open_input(&format, nullptr, nullptr, &options);
	const bool unusedPolicy = av_dict_count(options) != 0;
	av_dict_free(&options);
	if (input.ioError || (opened >= 0 && unusedPolicy))
		return 12;
	if (opened < 0)
		return interrupted(&input) ? (input.exhausted ? 11 : 13) : 11;
	// A reduced build must not publish an excluded container as a guessed raw
	// stream. Raw demuxers need at least the extension probe score (50).
	if (av_match_name(format->iformat->name, "aac,ac3,dts,eac3,gsm,h264,hevc,m4v,mpegvideo") &&
		format->probe_score < AVPROBE_SCORE_EXTENSION)
		return 11;
	// A partial stream-info result can still contain usable container metadata.
	avformat_find_stream_info(format, nullptr);
	if (input.ioError)
		return 12;
	if (interrupted(&input))
		return input.exhausted ? 11 : 13;
	if (std::ferror(stdin))
		return 12;
	if (format->duration != AV_NOPTS_VALUE)
		result.length = bounded(double(format->duration) / AV_TIME_BASE, true);
	result.bitrate = bounded(double(format->bit_rate) / 1000, false);
	const AVStream *selected = nullptr;
	unsigned audioCount = 0;
	for (unsigned i = 0; i < format->nb_streams; ++i) {
		const auto *stream = format->streams[i];
		if (stream->disposition & AV_DISPOSITION_ATTACHED_PIC)
			continue;
		const auto type = stream->codecpar->codec_type;
		if (type == AVMEDIA_TYPE_VIDEO) {
			selected = stream;
			break;
		}
		if (type == AVMEDIA_TYPE_AUDIO) {
			++audioCount;
			if (!selected)
				selected = stream;
		}
	}
	if (selected && selected->codecpar->codec_id != AV_CODEC_ID_NONE)
		result.codec = avcodec_get_name(selected->codecpar->codec_id);
	result.artist = tag(format->metadata, "artist");
	result.album = tag(format->metadata, "album");
	result.title = tag(format->metadata, "title");
	// RIFF INFO calls the album IPRD (libav's "product"). Prefer an explicit album.
	if (result.album.empty() && std::strcmp(format->iformat->name, "avi") == 0)
		result.album = tag(format->metadata, "product");
	// Preserve the previous backend's single-audio comment fallback, including
	// FLAC/Opus/Vorbis/Speex in Matroska. Ogg video also carries programme comments.
	// Other video containers can use stream title for a generic track label.
	const bool audioComments = selected && selected->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
				   audioCount == 1 &&
				   (result.codec == "flac" || result.codec == "opus" ||
					   result.codec == "vorbis" || result.codec == "speex");
	if (selected && (std::strcmp(format->iformat->name, "ogg") == 0 || audioComments)) {
		if (result.artist.empty())
			result.artist = tag(selected->metadata, "artist");
		if (result.album.empty())
			result.album = tag(selected->metadata, "album");
		if (result.title.empty())
			result.title = tag(selected->metadata, "title");
	}
	if (selected && selected->codecpar->codec_id == AV_CODEC_ID_MP3 &&
		std::strcmp(format->iformat->name, "mp3") == 0 &&
		format->duration_estimation_method == AVFMT_DURATION_FROM_BITRATE) {
		// Count complete frames when affordable; otherwise retain the container's
		// estimate. Tags, cover art, gaps and trailing bytes are not audio bitrate.
		double seconds = 0;
		uint64_t frameBytes = 0;
		if (scanMp3(format, selected, input, seconds, frameBytes)) {
			result.length = bounded(seconds, true);
			result.bitrate = bounded(double(frameBytes) * 8 / seconds / 1000, true);
		} else if (interrupted(&input)) {
			return input.exhausted ? 11 : 13;
		}
	}
	// Optional frame scanning can encounter a transient read failure too.
	if (input.ioError)
		return 12;
	return result.length || !result.codec.empty() ? 0 : 10;
}
void quoted(const std::string &text, bool json = true)
{
	std::cout << '"';
	for (unsigned char ch : text) {
		switch (ch) {
		case '"':
			std::cout << "\\\"";
			break;
		case '\\':
			std::cout << "\\\\";
			break;
		case '\n':
			std::cout << "\\n";
			break;
		case '\r':
			std::cout << "\\r";
			break;
		case '\t':
			std::cout << (json ? "\\t" : "\t");
			break;
		default:
			std::cout << ch;
		}
	}
	std::cout << '"';
}
void limits()
{
#ifndef _WIN32
	rlimit cpu{ 25, 25 }, writes{ 256 * 1024, 256 * 1024 }, core{ 0, 0 };
	setrlimit(RLIMIT_CPU, &cpu);
	setrlimit(RLIMIT_FSIZE, &writes);
	setrlimit(RLIMIT_CORE, &core);
#if !defined(__SANITIZE_ADDRESS__)
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
	return;
#endif
#endif
	rlimit memory{ 1024ULL * 1024 * 1024, 1024ULL * 1024 * 1024 };
	setrlimit(RLIMIT_AS, &memory);
#endif
#endif
}
} // namespace
int main(int argc, char **argv)
{
	std::setlocale(LC_NUMERIC, "C");
	bool json = false;
	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg == "--json")
			json = true;
		else if (arg == "--version") {
			std::cout << "amule-media-probe 1; FFmpeg " << av_version_info() << '\n';
			return 0;
		} else
			return 12;
	}
#ifdef _WIN32
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
	struct _stat64 statbuf
	{
	};
	if (_fstat64(_fileno(stdin), &statbuf) || (statbuf.st_mode & _S_IFMT) != _S_IFREG)
		return 12;
#else
	struct stat statbuf
	{
	};
	if (fstat(fileno(stdin), &statbuf) || !S_ISREG(statbuf.st_mode))
		return 12;
#endif
	limits();
	if (statbuf.st_size < 0 || reposition(0, SEEK_SET))
		return 12;
	av_log_set_level(AV_LOG_QUIET);
	av_max_alloc(128 * 1024 * 1024);
	Input input;
	input.size = statbuf.st_size;
	if (!json)
		std::cout << "AMMP 1\n" << std::flush;
	Result result;
	int status = 12;
	try {
		status = probe(input, result);
	} catch (...) {
		return 12;
	}
	if (status)
		return status;
	if (json) {
		std::cout << "{\"backend\":\"libav\",\"length_seconds\":" << result.length
			  << ",\"bitrate_kbps\":" << result.bitrate << ",\"codec\":";
		quoted(result.codec);
		for (const auto &item : { std::make_pair("artist", result.artist),
			     { "album", result.album },
			     { "title", result.title } }) {
			std::cout << ",\"" << item.first << "\":";
			quoted(item.second);
		}
		std::cout << "}\n";
	} else {
		std::cout << "format.duration=" << result.length
			  << "\nformat.bit_rate=" << uint64_t(result.bitrate) * 1000
			  << "\nstreams.stream.0.codec_type=\"audio\"\nstreams.stream.0.codec_name=";
		quoted(result.codec);
		std::cout << '\n';
		for (const auto &item : { std::make_pair("artist", result.artist),
			     { "album", result.album },
			     { "title", result.title } }) {
			std::cout << "format.tags." << item.first << '=';
			quoted(item.second, false);
			std::cout << '\n';
		}
		std::cout << "END\n";
	}
	return 0;
}
