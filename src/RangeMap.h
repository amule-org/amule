//
// This file is part of the aMule Project.
//
// Copyright (c) 2004-2011 Mikkel Schubert ( xaignar@users.sourceforge.net )
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

#ifndef RANGEMAP_H
#define RANGEMAP_H

#include <map>

#include <common/MuleDebug.h>

/**
 * Default helper structure for normal CRangeMap instantiations.
 *
 * A specialization must provide the four value typedefs (which fix the iterators' return types), a
 * template-defined member named 'first', and a comparison operator that ignores 'first'.
 */
template <typename VALUE, typename KEYTYPE> struct CRangeMapHelper
{
	typedef VALUE *ValuePtr;
	typedef VALUE &ValueRef;
	typedef const VALUE &ConstValueRef;
	typedef const VALUE *ConstValuePtr;

	//! Used internally by CRangeMap to specify the end of a range.
	KEYTYPE first;
	//! Contains the value of a given range.
	VALUE second;

	bool operator==(const CRangeMapHelper<VALUE, KEYTYPE> &o) const { return second == o.second; }
};

/// Helper structure for CRangeSet (CRangeMap with void as value).
template <typename KEYTYPE> struct CRangeMapHelper<void, KEYTYPE>
{
	typedef void ValuePtr;
	typedef void ValueRef;
	typedef void ConstValueRef;
	typedef void ConstValuePtr;

	KEYTYPE first;

	bool operator==(const CRangeMapHelper<void, KEYTYPE> &) const { return true; }
};

/**
 * A map of non-overlapping ranges, each with a user-specified value.
 *
 * Supports quick lookup of which range covers a key, and merges or splits existing ranges as new
 * ones are added. Whether to split/resize or merge is decided by the equality operator on the user
 * value: two equal-valued ranges that overlap or sit adjacent are merged into one, and if the
 * values differ the old range is resized or split around the new one. A split copies the user value
 * into both parts.
 *
 * Existing ranges cannot be edited in place -- erase and re-insert. A specialization typedef'd as
 * CRangeSet associates no value with a range.
 *
 * NOTE: KEYTYPE is assumed to be an unsigned integer type.
 */
template <typename VALUE, typename KEYTYPE = uint64> class CRangeMap
{
	typedef CRangeMapHelper<VALUE, KEYTYPE> HELPER;

private:
	//! The map uses the start-key as key and the User-value and end-key pair as value
	typedef std::map<KEYTYPE, HELPER> RangeMap;
	typedef std::pair<KEYTYPE, HELPER> RangePair;

	//! Typedefs used to distinguish between our custom iterator and the real ones.
	typedef typename RangeMap::iterator RangeIterator;
	typedef typename RangeMap::const_iterator ConstRangeIterator;

	RangeMap m_ranges;

	/**
	 * Wraps the raw iterators CRangeMap uses: acts as a normal iterator returning the user
	 * value for the range, plus keyStart() and keyEnd().
	 */
	template <typename RealIterator, typename ReturnTypeRef, typename ReturnTypePtr> class iterator_base
	{
		friend class CRangeMap<VALUE, KEYTYPE>;

	public:
		iterator_base(const RealIterator &it)
		: m_it(it)
		{
		}

		bool operator==(const iterator_base &other) const { return m_it == other.m_it; }

		bool operator!=(const iterator_base &other) const { return m_it != other.m_it; }

		//! Returns the starting point of the range
		KEYTYPE keyStart() const { return m_it->first; }

		//! Returns the end-point of the range
		KEYTYPE keyEnd() const { return m_it->second.first; }

		iterator_base &operator++()
		{
			++m_it;

			return *this;
		}

		iterator_base operator++(int) { return iterator_base(m_it++); }

		iterator_base &operator--()
		{
			--m_it;

			return *this;
		}

		iterator_base operator--(int) { return iterator_base(m_it--); }

		ReturnTypeRef operator*() const { return m_it->second.second; }

		ReturnTypePtr operator->() const { return &m_it->second.second; }

	protected:
		RealIterator m_it;
	};

	typedef typename HELPER::ValueRef ValueRef;
	typedef typename HELPER::ValuePtr ValuePtr;
	typedef typename HELPER::ConstValueRef ConstValueRef;
	typedef typename HELPER::ConstValuePtr ConstValuePtr;

public:
	typedef iterator_base<RangeIterator, ValueRef, ValuePtr> iterator;
	typedef iterator_base<ConstRangeIterator, ConstValueRef, ConstValuePtr> const_iterator;

	typedef typename RangeMap::size_type size_type;

	typedef VALUE value_type;

	CRangeMap() {}

	CRangeMap(const CRangeMap<VALUE, KEYTYPE> &other)
	: m_ranges(other.m_ranges)
	{
	}

	CRangeMap &operator=(const CRangeMap<VALUE, KEYTYPE> &other)
	{
		m_ranges = other.m_ranges;

		return *this;
	}

	/// Swaps the contents of the two rangemaps.
	void swap(CRangeMap<VALUE, KEYTYPE> &other) { std::swap(m_ranges, other.m_ranges); }

	/// True if both maps hold the same ranges and values.
	bool operator==(const CRangeMap<VALUE, KEYTYPE> &other) const
	{
		if (this == &other) {
			return true;
		}

		if (size() != other.size()) {
			return false;
		}

		return (m_ranges == other.m_ranges);
	}

	/// Iterator to the first range.
	iterator begin() { return m_ranges.begin(); }

	/// Iterator past the last range.
	iterator end() { return m_ranges.end(); }

	/// Const iterator to the first range.
	const_iterator begin() const { return m_ranges.begin(); }

	/// Const iterator past the last range.
	const_iterator end() const { return m_ranges.end(); }

	/// Erases the range at @a pos and returns the iterator of the range after it. Erasing end()
	/// is invalid.
	iterator erase(iterator pos)
	{
		MULE_VALIDATE_PARAMS(pos != end(), "Cannot erase 'end'");

		RangeIterator temp = pos.m_it++;

		m_ranges.erase(temp);

		return pos;
	}

	/// Number of ranges in the map.
	size_type size() const { return m_ranges.size(); }

	/// True if the map is empty.
	bool empty() const { return m_ranges.empty(); }

	/// Removes all ranges from the map.
	void clear() { m_ranges.clear(); }

	/// The range covering @a key (it->first <= key <= it->second->first), or end(). A range
	/// covers a value from its start-key to its end-key inclusive.
	iterator find_range(KEYTYPE key)
	{
		if (!m_ranges.empty()) {
			// Find first range whose start comes after key
			// Thus: key < it->first, but (--it)->first <= key
			RangeIterator it = m_ranges.upper_bound(key);

			if (it != m_ranges.begin()) {
				--it;

				if (key <= it->second.first) {
					return it;
				}
			}
		}

		return end();
	}

	void erase_range(KEYTYPE startPos, KEYTYPE endPos)
	{
		// Create default initialized entry, which ensures that all fields are initialized.
		HELPER entry = HELPER();
		entry.first = endPos;

		// Insert without merging, which forces the creation of an entry that
		// only covers the specified range, which will crop existing ranges.
		erase(do_insert(startPos, entry, false));
	}

	/**
	 * Inserts a new range, overwriting or resizing existing ranges on conflict.
	 *
	 * @param startPos Start of the range, inclusive. Must be <= endPos.
	 * @param endPos End of the range, inclusive.
	 * @param object The user-data to associate with the range.
	 * @return An iterator covering at least the specified range.
	 *
	 * Ranges whose objects compare equal are merged, including ones placed directly before or
	 * after each other, so the iterator returned can point to a range quite different from the one
	 * specified. Make VALUE compare unequal always if that is unwanted; otherwise the only promise
	 * is that the resulting range carries the same user-data.
	 */
	//@{
	iterator insert(KEYTYPE startPos, KEYTYPE endPos)
	{
		HELPER entry = { endPos };
		return do_insert(startPos, entry);
	}
	template <typename TYPE> iterator insert(KEYTYPE startPos, KEYTYPE endPos, const TYPE &value)
	{
		HELPER entry = { endPos, value };
		return do_insert(startPos, entry);
	}
	//@}

protected:
	/// Inserts the range starting at @a start and described by @a entry (end position and any
	/// user-data), merging with neighbours when @a merge. Returns an iterator covering at least
	/// that range.
	iterator do_insert(KEYTYPE start, HELPER entry, bool merge = true)
	{
		MULE_VALIDATE_PARAMS(start <= entry.first, "Not a valid range.");

		RangeIterator it = get_insert_it(start);
		while (it != m_ranges.end()) {
			// Begins before the current span
			if (start <= it->first) {
				// Never touches the current span, it follows that start < it->first
				// (it->first) is used to avoid checking against (uintXX)-1 by accident
				if (entry.first < it->first - 1 && it->first) {
					break;
				}

				// Stops just before the current span, it follows that start < it->first
				// (it->first) is used to avoid checking against (uintXX)-1 by accident
				else if (entry.first == it->first - 1 && it->first) {
					// If same type: Merge
					if (merge && (entry == it->second)) {
						entry.first = it->second.first;
						m_ranges.erase(it++);
					}

					break;
				}

				// Covers part of the span
				else if (entry.first < it->second.first) {
					// Same type, merge
					if (merge && (entry == it->second)) {
						entry.first = it->second.first;
						m_ranges.erase(it++);
					} else {
						// Resize the partially covered span and get the next one
						it = ++resize(entry.first + 1, it->second.first, it);
					}

					break;
				} else {
					// It covers the entire span
					m_ranges.erase(it++);
				}
			}

			// Starts inside the current span or after the current span
			else {
				// Starts inside the current span
				if (start <= it->second.first) {
					// Ends inside the current span
					if (entry.first < it->second.first) {
						// Adding a span with same type inside a existing span is
						// fruitless
						if (merge && (entry == it->second)) {
							return it;
						}

						// Insert the new span
						m_ranges.insert(it, RangePair(entry.first + 1, it->second));

						// Resize the current span to fit before the new span
						it->second.first = start - 1;

						break;
					} else {
						// Ends past the current span, resize or merge
						if (merge && (entry == it->second)) {
							start = it->first;
							m_ranges.erase(it++);
						} else {
							// Resize the partially covered span and get the next
							// one
							it = ++resize(it->first, start - 1, it);
						}
					}
				} else {
					// Start past the current span
					if (start == it->second.first + 1) {
						// Touches the current span
						if (merge && (entry == it->second)) {
							start = it->first;
							m_ranges.erase(it++);
						} else {
							++it;
						}
					} else {
						// Starts after the current span, nothing to do
						++it;
					}
				}
			}
		}

		return m_ranges.insert(it, RangePair(start, entry));
	}

	/// Finds the optimal place to start looking for insertion points: the first range whose
	/// start comes after the new start. The last element is checked first, since sequential
	/// insertions are common.
	RangeIterator get_insert_it(KEYTYPE start)
	{
		if (m_ranges.empty()) {
			return m_ranges.end();
		}

		// The start-key of the last element must be smaller than our start-key
		// Otherwise there is the possibility that we can merge with the one before that
		RangeIterator it = --m_ranges.end();
		if (start <= it->first) {
			// If the two starts are equal, then we only need to go back another
			// step to see if the range prior to this one is mergeable
			if (start != it->first) {
				it = m_ranges.lower_bound(start);
			}

			if (it != m_ranges.begin()) {
				// Go back to the last range which starts at or before key
				--it;
			}
		}

		return it;
	}

	//! Helper function that resizes an existing range to the specified size.
	RangeIterator resize(KEYTYPE startPos, KEYTYPE endPos, RangeIterator it)
	{
		HELPER item = it->second;
		item.first = endPos;

		m_ranges.erase(it++);

		return m_ranges.insert(it, RangePair(startPos, item));
	}
};

//! CRangeSet is simply a partial specialization of CRangeMap
typedef CRangeMap<void> CRangeSet;

namespace std
{
/** @see CRangeMap::swap */
template <typename VALUE_TYPE, typename KEY_TYPE>
void swap(CRangeMap<VALUE_TYPE, KEY_TYPE> &a, CRangeMap<VALUE_TYPE, KEY_TYPE> &b)
{
	a.swap(b);
}
} // namespace std

#endif
// File_checked_for_headers
