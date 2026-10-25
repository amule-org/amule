// Copyright (c) 2026 aMule Team
// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef PARTFILEBUFFERING_H
#define PARTFILEBUFFERING_H

#include <cstdint>
#include <map>

// Keep pending writes in ascending file-offset order. multimap inserts after equal keys,
// so equal ends retain arrival order.
template <typename Entry>
void InsertPartFileBufferedWrite(std::multimap<uint64_t, Entry *> &buffers, Entry *item)
{
	buffers.emplace(item->end, item);
}
#endif
