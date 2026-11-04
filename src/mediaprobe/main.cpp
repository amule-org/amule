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
	Clock::time_point deadline = Clock::now() + std::chrono::seconds(25);
};
struct Result
{
	uint32_t length = 0, bitrate = 0;
	std::string codec, artist, album, title;
};
// Layer III frame duration is fixed by its MPEG version/sample rate, not bitrate.
// Validate complete packets before counting: a parser flush can expose a truncated
// last frame, and resynchronisation can otherwise hide damaged bytes in the middle.
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
bool scanMp3(AVFormatContext *format, const AVStream *stream, const Input &input, double &seconds)
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
	int64_t total = 0, end = -1;
	int status = 0;
	// find_stream_info buffers the initial packets: read_frame starts at the
	// logical beginning without seeking away encoder-delay or cover-art state.
	while ((status = av_read_frame(format, packet)) >= 0) {
		if (packet->stream_index == stream->index) {
			int64_t ticks = 0;
			if (!mp3FrameTicks(*packet, ticks) || packet->pos < 0 ||
				packet->pos > input.size - packet->size || (end >= 0 && packet->pos != end) ||
				total > std::numeric_limits<int64_t>::max() - ticks)
				return false;
			total += ticks;
			end = packet->pos + packet->size;
		}
		av_packet_unref(packet);
	}
	if (status != AVERROR_EOF || input.exhausted || Clock::now() >= input.deadline ||
		std::ferror(stdin) || !total || end < 0)
		return false;
	if (end != input.size) {
		// Accept the well-defined ID3v1 footer. Unknown trailing bytes, damaged
		// frames and unsupported footer layouts must not become counted duration.
		unsigned char footer[128];
		if (input.size - end != 128 || avio_seek(format->pb, end, SEEK_SET) != end ||
			avio_read(format->pb, footer, sizeof(footer)) != sizeof(footer) ||
			std::memcmp(footer, "TAG", 3) != 0)
			return false;
	}
	if (input.exhausted || Clock::now() >= input.deadline)
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
	if (count)
		return static_cast<int>(count);
	return std::ferror(stdin) ? AVERROR(EIO) : AVERROR_EOF;
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
	if (current < 0 || (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) ||
		offset < -base || offset > input.size - base)
		return AVERROR(EINVAL);
	const auto target = base + offset;
	if (target <= current && ++input.backwardSeeks > MaxBackwardSeeks) {
		input.exhausted = true;
		return AVERROR(EFBIG);
	}
	return reposition(target, SEEK_SET) ? AVERROR(EIO) : target;
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
	while (cursor < end && result.size() < 4096) {
		const auto *start = cursor;
		int32_t codepoint = 0;
		if (av_utf8_decode(&codepoint, &cursor, end, 0) < 0) {
			cursor = start + 1;
			if (result.size() + 3 <= 4096)
				result += "\xef\xbf\xbd";
			continue;
		}
		if (codepoint < 32 && codepoint != '\t' && codepoint != '\n' && codepoint != '\r')
			continue;
		if (codepoint == 127 || (codepoint >= 128 && codepoint <= 159))
			continue;
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
	av_dict_set(&options, "protocol_whitelist", "", 0);
	// Keep this policy in code: distro FFmpeg enables network and playlist readers.
	// Match packaging/media/build-static.sh, including libavformat's runtime aliases.
	av_dict_set(&options,
		"format_whitelist",
		"aac,ac3,aiff,amr,ape,asf,avi,dts,eac3,flac,flv,gsm,h264,hevc,matroska,webm,"
		"mov,mp4,m4a,3gp,3g2,mj2,mp3,mpeg,mpegts,mpegvideo,ogg,wav,wv,tta",
		0);
	// find_stream_info may otherwise open decoders in a full system FFmpeg.
	// Container metadata and parsers suffice; match the decoder-free bundle.
	av_dict_set(&options, "codec_whitelist", "", 0);
	const int opened = avformat_open_input(&format, nullptr, nullptr, &options);
	av_dict_free(&options);
	if (opened < 0)
		return interrupted(&input) ? (input.exhausted ? 11 : 13) : 11;
	// A partial stream-info result can still contain usable container metadata.
	(void)avformat_find_stream_info(format, nullptr);
	if (interrupted(&input))
		return input.exhausted ? 11 : 13;
	if (std::ferror(stdin))
		return 12;
	if (format->duration != AV_NOPTS_VALUE)
		result.length = bounded(double(format->duration) / AV_TIME_BASE, true);
	result.bitrate = bounded(double(format->bit_rate) / 1000, false);
	const AVStream *selected = nullptr;
	for (unsigned i = 0; i < format->nb_streams; ++i) {
		const auto *stream = format->streams[i];
		if (stream->disposition & AV_DISPOSITION_ATTACHED_PIC)
			continue;
		const auto type = stream->codecpar->codec_type;
		if (type == AVMEDIA_TYPE_VIDEO) {
			selected = stream;
			break;
		}
		if (!selected && type == AVMEDIA_TYPE_AUDIO)
			selected = stream;
	}
	if (selected && selected->codecpar->codec_id != AV_CODEC_ID_NONE)
		result.codec = avcodec_get_name(selected->codecpar->codec_id);
	result.artist = tag(format->metadata, "artist");
	result.album = tag(format->metadata, "album");
	result.title = tag(format->metadata, "title");
	// RIFF INFO calls the album IPRD (libav's "product"). Prefer an explicit album.
	if (result.album.empty() && std::strcmp(format->iformat->name, "avi") == 0)
		result.album = tag(format->metadata, "product");
	// Ogg video also carries programme comments on its selected stream.
	// Other containers can use stream title for a track label (e.g. "VP8 video stream").
	if (selected && std::strcmp(format->iformat->name, "ogg") == 0) {
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
		// A sampled bitrate cannot establish VBR duration without Xing/VBRI.
		// Keep other fields if the optional bounded scan cannot establish a total.
		result.length = 0;
		result.bitrate = 0;
		double seconds = 0;
		if (scanMp3(format, selected, input, seconds)) {
			result.length = bounded(seconds, true);
			result.bitrate = bounded(double(input.size) * 8 / seconds / 1000, false);
		}
	}
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
