// This file is part of the aMule Project.
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Licensed under the GNU General Public License, version 2 or later.

#ifndef KADPUBLISHCURSOR_H
#define KADPUBLISHCURSOR_H

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

// Visits every key once per pass. Files close in hash order share leading hash bits, so their
// sources land on the same Kad nodes, which refuse more than 2 a minute from one sender. Each
// pass therefore puts the j-th key in hash order at slot j * T mod N, with T near N / golden
// ratio: neighbours are 0.38 N slots apart, keys within 3 places at least 0.14 N.
// Keys, not iterators, so removals and reloads cannot invalidate the cursor: removed keys are
// skipped, new keys join the next pass.
template <typename Key> class CKadPublishCursor
{
public:
	template <typename Map> typename Map::const_iterator Next(const Map &files)
	{
		// The second round starts a new pass when the current one is used up.
		for (int round = 0; round < 2; ++round) {
			while (m_position < m_order.size()) {
				const auto next = files.find(m_order[m_position++]);
				if (next != files.end()) {
					return next;
				}
			}
			StartPass(files);
		}
		return files.end();
	}

private:
	template <typename Map> void StartPass(const Map &files)
	{
		const size_t count = files.size();
		m_order.resize(count);
		m_position = 0;
		if (count == 0) {
			return;
		}
		size_t step = std::max<size_t>(
			1, static_cast<size_t>(std::lround(static_cast<double>(count) * 0.6180339887)));
		while (std::gcd(step, count) != 1) {
			++step;
		}
		size_t slot = 0;
		for (const auto &file : files) {
			m_order[slot] = file.first;
			slot = (slot + step) % count;
		}
	}

	std::vector<Key> m_order;
	size_t m_position = 0;
};

#endif // KADPUBLISHCURSOR_H
