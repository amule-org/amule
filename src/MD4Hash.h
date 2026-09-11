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

#ifndef CMD4HASH_H
#define CMD4HASH_H

#include "ArchSpecific.h" // Needed for Raw{Peek,Poke}UInt64()

#include <common/MuleDebug.h> // Needed for MULE_VALIDATE_PARAMS

#ifdef USE_WX_EXTENSIONS
#include <common/StringFunctions.h>
#endif

#include <string>

const size_t MD4HASH_LENGTH = 16;

/**
 * Container class for the MD4 hashes used in aMule.
 *
 * Transparently wraps the char array the hash is stored in, so assignment, equality and other
 * operators come for free. The hashes are arrays of length 16 WITHOUT a zero terminator.
 */
class CMD4Hash
{
public:
	/**
	 * Creates an empty hash of length 16, every field zero.
	 */
	CMD4Hash() { Clear(); }

	/**
	 * Casts an unsigned char array to a CMD4Hash. @a hash must either be NULL or at least 16
	 * chars long, not counting any zero terminator.
	 */
	explicit CMD4Hash(const unsigned char hash[]) { SetHash(hash); }

	/**
	 * True if all fields of both hashes are the same.
	 */
	bool operator==(const CMD4Hash &other_hash) const
	{
		return ((RawPeekUInt64(m_hash) == RawPeekUInt64(other_hash.m_hash)) &&
			(RawPeekUInt64(m_hash + 8) == RawPeekUInt64(other_hash.m_hash + 8)));
	}

	/**
	 * True if the two hashes differ in any field.
	 */
	bool operator!=(const CMD4Hash &other_hash) const { return !(*this == other_hash); }

	/**
	 * True if this hash sorts before @a other_hash, so CMD4Hashes can be used in sorted STL
	 * containers like std::map.
	 */
	bool operator<(const CMD4Hash &other_hash) const
	{
		for (size_t i = 0; i < MD4HASH_LENGTH; ++i) {
			if (m_hash[i] < other_hash.m_hash[i]) {
				return true;
			} else if (other_hash.m_hash[i] < m_hash[i]) {
				return false;
			}
		}

		return false;
	}

	/**
	 * True if every field of the hash is zero. Clear() produces such a hash.
	 */
	bool IsEmpty() const { return (!RawPeekUInt64(m_hash) && !RawPeekUInt64(m_hash + 8)); }

	/**
	 * Sets every field of the hash to zero, so IsEmpty() then returns true.
	 */
	void Clear()
	{
		RawPokeUInt64(m_hash, 0);
		RawPokeUInt64(m_hash + 8, 0);
	}

	/**
	 * Decodes a 32-char hexadecimal representation of an MD4 hash into m_hash. Returns whether
	 * it decoded.
	 */

	bool Decode(const std::string &hash)
	{
		if (hash.length() != MD4HASH_LENGTH * 2) {
			return false;
		}

		for (size_t i = 0; i < MD4HASH_LENGTH * 2; i++) {
			unsigned char word = toupper(hash[i]);

			if ((word >= '0') && (word <= '9')) {
				word -= '0';
			} else if ((word >= 'A') && (word <= 'F')) {
				word -= 'A' - 10;
			} else {
				// Invalid chars
				return false;
			}

			if (i % 2 == 0) {
				m_hash[i / 2] = word << 4;
			} else {
				m_hash[i / 2] += word;
			}
		}

		return true;
	}

#ifdef USE_WX_EXTENSIONS
	bool Decode(const wxString &hash) { return Decode(std::string(unicode2char(hash))); }
#endif

	/**
	 * The 32-char hexadecimal representation of the hash in m_hash.
	 */
	std::string EncodeSTL() const
	{
		std::string Base16Buff;

		for (size_t i = 0; i < MD4HASH_LENGTH * 2; i++) {
			size_t x = (i % 2 == 0) ? (m_hash[i / 2] >> 4) : (m_hash[i / 2] & 0xf);

			if (x < 10) {
				Base16Buff += (char)(x + '0');
			} else {
				Base16Buff += (char)(x + ('A' - 10));
			}
		}

		return Base16Buff;
	}

#ifdef USE_WX_EXTENSIONS
	wxString Encode() const { return char2unicode(EncodeSTL().c_str()); }
#endif

	/**
	 * Explicitly sets the hash array from @a hash, which must be NULL or of length 16.
	 */
	void SetHash(const unsigned char hash[])
	{
		if (hash) {
			RawPokeUInt64(m_hash, RawPeekUInt64(hash));
			RawPokeUInt64(m_hash + 8, RawPeekUInt64(hash + 8));
		} else {
			Clear();
		}
	}

	/**
	 * Pointer to the hash array.
	 */
	unsigned char *GetHash() { return m_hash; }
	const unsigned char *GetHash() const { return m_hash; }

	/**
	 * The value at index @a i of the hash array, or a reference to it.
	 */
	unsigned char operator[](size_t i) const
	{
		MULE_VALIDATE_PARAMS(i < MD4HASH_LENGTH, "Invalid index in CMD4Hash::operator[]");
		return m_hash[i];
	}

	unsigned char &operator[](size_t i)
	{
		MULE_VALIDATE_PARAMS(i < MD4HASH_LENGTH, "Invalid index in CMD4Hash::operator[]");
		return m_hash[i];
	}

private:
	//! The raw MD4 hash. In most cases prefer the member functions to direct access. Value-
	//! initialised so the buffer is never read uninitialised even if a future ctor forgets to
	//! set it; the analyzer also cannot see through the RawPokeUInt64 pokes in Clear()/SetHash,
	//! so this keeps it quiet too.
	unsigned char m_hash[MD4HASH_LENGTH] = {};
};

namespace std
{
//! Lets a hash key an unordered container, for the O(1) lookups a per-tick reconcile needs. MD4
//! output is already uniformly distributed, so two of its words mixed make a better bucket index
//! than anything computed over them.
template <> struct hash<CMD4Hash>
{
	size_t operator()(const CMD4Hash &value) const noexcept
	{
		const size_t low = static_cast<size_t>(RawPeekUInt64(value.GetHash()));
		const size_t high = static_cast<size_t>(RawPeekUInt64(value.GetHash() + 8));
		return low ^ (high << 1);
	}
};
} // namespace std

#endif
// File_checked_for_headers
