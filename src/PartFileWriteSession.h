// Copyright (c) 2026 aMule Team
// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef PARTFILEWRITESESSION_H
#define PARTFILEWRITESESSION_H
#include <atomic>
#include <cstdint>
#include <string>
#include <common/Path.h>

struct PartFileWriteSession
{
	inline static std::atomic<uint64_t> next{ 0 };
	const uint64_t id = ++next;
	std::atomic<bool> cancelled{ false };
	std::atomic<bool> preparing{ false }, ready{ false };
	// Written once by discovery, before publishing the resolved job under the
	// pool lock. Every subsequent batch observes these immutable fields.
	std::string volume;
	CPath spacePath;
};
#endif
