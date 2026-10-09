//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2002-2011 Merkur ( devs@emule-project.net / http://www.emule-project.net )
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

#ifndef DEADSOURCELIST_H
#define DEADSOURCELIST_H

#include <map>

#include "DeadSourceKey.h"
#include "Types.h"

class CUpDownClient;

/**
 * Tracks "invalid" sources.
 *
 * A dead source is one evaluated as useless, for instance because it does not answer queries.
 * Listing it lets it be ignored for a set time, avoiding the overhead of trying to connect -- which
 * matters, since such sources would otherwise be removed and re-added repeatedly for no gain.
 */
class CDeadSourceList
{
public:
	/**
	 * @param isGlobal Whether the list is global; used for debugging.
	 */
	CDeadSourceList(bool isGlobal = false);

	/**
	 * Adds a client to the list of dead sources.
	 */
	void AddDeadSource(const CUpDownClient *client);

	/**
	 * True if the client object is a dead source.
	 */
	bool IsDeadSource(const CUpDownClient *client);

	/**
	 * The number of sources.
	 */
	uint32 GetDeadSourcesCount() const;

private:
	/**
	 * Removes entries that are too old.
	 */
	void CleanUp();

	/**
	 * Record of a dead source.
	 */
	class CDeadSource
	{
	public:
		explicit CDeadSource(const CDeadSourceKey &key);

		/**
		 * Equality operator.
		 */
		bool operator==(const CDeadSource &other) const;

		/**
		 * Sets the timestamp at which this entry expires.
		 */
		void SetTimeout(uint64 t);

		/**
		 * The timestamp of this entry.
		 */
		uint64 GetTimeout() const;

	private:
		CDeadSourceKey m_key;
		//! The timestamp of DOOM!
		uint64 m_TimeStamp;
	};

	typedef std::multimap<CDeadSourceKey, CDeadSource> DeadSourceMap;
	typedef DeadSourceMap::iterator DeadSourceIterator;
	typedef std::pair<DeadSourceIterator, DeadSourceIterator> DeadSourcePair;
	//! List of currently dead sources.
	DeadSourceMap m_sources;

	//! The timestamp of when the last cleanup was performed.
	uint64 m_dwLastCleanUp;
	//! Specifies if the list is global or not.
	bool m_bGlobalList;
};

#endif
// File_checked_for_headers
