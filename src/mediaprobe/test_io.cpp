// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the production AVIO callbacks without replacing the parser library.
#define main mediaProbeMain
#include "main.cpp"
#undef main

int main()
{
	Input input;
	input.size = 4096;
	// Seeking before byte zero or beyond EOF must not reach stdio.
	if (seekInput(&input, -1, SEEK_SET) != AVERROR(EINVAL) ||
		seekInput(&input, 4097, SEEK_SET) != AVERROR(EINVAL))
		return 1;
	if (seekInput(&input, 0, AVSEEK_SIZE) != 4096)
		return 2;
	uint8_t byte = 0;
	input.bytes = MaxRead;
	if (readInput(&input, &byte, 1) != AVERROR(EFBIG) || !input.exhausted)
		return 3;
	Input expired;
	expired.deadline = Clock::now() - std::chrono::seconds(1);
	if (readInput(&expired, &byte, 1) != AVERROR_EXIT || !interrupted(&expired))
		return 4;
	return 0;
}
