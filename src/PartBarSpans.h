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

#ifndef PARTBARSPANS_H
#define PARTBARSPANS_H

// Which byte range each part of a file occupies in a chunk bar, and which of the
// two things the shared-files bar is drawing.
//
// Extracted because the arithmetic was wrong and nothing could see it. The three
// bar columns computed their own spans, and CSharedFilesCtrl computed them
// differently from the other two -- an end one byte past where CBarShader wants
// it (src/BarShader.cpp:113-124 treats end as inclusive and increments it
// itself). Invisible at MiB scale and unassertable while it lived inside a
// method that needs a CKnownFile and a wxDC.
//
// Header-only, wx-free and constexpr on purpose, like PartBarLegend.h: the
// geometry and the mode choice are decided from two integers, so a headless run
// can check them.

#include <cstddef>
#include <cstdint>

namespace partbar
{

//! One part's byte range, both ends inclusive, as CBarShader::FillRange reads
//! them.
struct Span
{
	std::uint64_t start = 0;
	std::uint64_t end = 0;
};

constexpr bool operator==(const Span &a, const Span &b)
{
	return a.start == b.start && a.end == b.end;
}

constexpr bool operator!=(const Span &a, const Span &b)
{
	return !(a == b);
}

//! What the shared-files bar is drawing for one row. The two are unrelated: one
//! is availability across the swarm, the other is local re-hash progress that
//! happens to occupy the same cell.
enum class BarMode
{
	None = 0,     //!< nothing to draw; the file reports no parts
	Availability, //!< source count per part
	Hashing       //!< re-hash progress over local data
};

/**
 * The byte range of part @p index.
 *
 * @param index     zero-based part index, expected < partCount.
 * @param partSize  bytes per part (PARTSIZE at every call site).
 * @param fileSize  total bytes; the last part is short whenever the file does
 *                  not divide evenly, and its end is clamped to the last byte
 *                  rather than running past it.
 *
 * Ends are inclusive and adjacent spans do not overlap, so span(i).end is
 * exactly span(i+1).start - 1. That property is what the old arithmetic broke.
 */
constexpr Span SpanFor(std::size_t index, std::uint64_t partSize, std::uint64_t fileSize)
{
	const std::uint64_t start = partSize * static_cast<std::uint64_t>(index);
	const std::uint64_t past = start + partSize;
	return Span{ start, (past < fileSize ? past : fileSize) - 1 };
}

/**
 * Which mode the bar is in.
 *
 * A file reporting no parts draws nothing at all, whatever progress says: the
 * caller has no span to fill, and a hashing bar over zero parts would be a bar
 * over nothing. Progress above zero otherwise means a re-hash is running.
 */
constexpr BarMode ModeFor(std::uint64_t hashedPartCount, std::size_t partCount)
{
	return partCount == 0 ? BarMode::None
			      : (hashedPartCount > 0 ? BarMode::Hashing : BarMode::Availability);
}

/**
 * How many parts of @p partCount are already hashed, clamped.
 *
 * CHashingTask reports part + 1 (src/ThreadTasks.cpp:179, :693), so a completed
 * pass reports one past the last part and the raw number cannot be used as an
 * index or a span count.
 */
constexpr std::size_t HashedPartsClamped(std::uint64_t hashedPartCount, std::size_t partCount)
{
	return hashedPartCount > static_cast<std::uint64_t>(partCount)
		       ? partCount
		       : static_cast<std::size_t>(hashedPartCount);
}

} // namespace partbar

#endif // PARTBARSPANS_H
