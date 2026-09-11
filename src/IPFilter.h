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

#ifndef IPFILTER_H
#define IPFILTER_H

#include <wx/event.h> // Needed for wxEvent

#include "Types.h" // Needed for uint8, uint16 and uint32

class CIPFilterEvent;

/**
 * A list of IPs that must not be accepted as connection destinations or sources, with an interface
 * to ask whether a given IP is filtered.
 *
 * Handles IPRange files in the Peer-Guardian and AntiP2P formats, read from plain text files or
 * from text files zip-compressed. Thread-safe.
 */
class CIPFilter : public wxEvtHandler
{
public:
	CIPFilter();

	/**
	 * True if @a IP2test is filtered by the current list and access level. @a isServer says
	 * whether the IP belongs to a server or a client, for statistics only. @a IP2test must be
	 * in anti-host order (BE on an LE platform, LE on a BE one).
	 */
	bool IsFiltered(uint32 IP2test, bool isServer = false);

	/**
	 * The number of banned ranges.
	 */
	uint32 BanCount() const;

	/**
	 * Reloads the ipfilter files, discarding the current list of ranges.
	 */
	void Reload();

	/**
	 * Starts a download of the ipfilter list at @a strURL. Once it has downloaded, ipfilter.dat
	 * is replaced with the new file and Reload is called.
	 */
	void Update(const wxString &strURL);

	/**
	 * Called when a download completes.
	 */
	void DownloadFinished(uint32 result);

	/**
	 * True once initial startup has finished; stays true while reloading later.
	 */
	bool IsReady() const { return m_ready; }

	/**
	 * Tells the filter to start these networks once it has finished loading.
	 */
	void StartKADWhenReady() { m_startKADWhenReady = true; }
	void ConnectToAnyServerWhenReady() { m_connectToAnyServerWhenReady = true; }

	/**
	 * Starts whichever networks the two flags above requested, and clears them. Called from the
	 * load-finished handler, and again once the flags have been set during startup: loading
	 * runs on a worker thread, so it can finish -- and its event be dispatched -- before the
	 * caller gets round to asking for a network. Doing nothing when no flag is set makes the
	 * second call harmless.
	 */
	void StartPendingNetworks();

private:
	/** Handles the result of loading the dat-files. */
	void OnIPFilterEvent(CIPFilterEvent &);

	//! The URL from which the IP filter was downloaded
	wxString m_URL;

	// The IP ranges
	typedef std::vector<uint32> RangeIPs;
	RangeIPs m_rangeIPs;
	typedef std::vector<uint16> RangeLengths;
	RangeLengths m_rangeLengths;
	// Name for each range. This usually stays empty for memory reasons,
	// except if IP-Filter debugging is active.
	typedef std::vector<std::string> RangeNames;
	RangeNames m_rangeNames;

	//! Mutex used to ensure thread-safety of this class
	mutable wxMutex m_mutex;

	// false if loading (on startup only)
	bool m_ready;
	// flags to start networks after loading
	bool m_startKADWhenReady;
	bool m_connectToAnyServerWhenReady;
	// should update be performed after filter is loaded ?
	bool m_updateAfterLoading;

	friend class CIPFilterEvent;
	friend class CIPFilterTask;

	wxDECLARE_EVENT_TABLE();
};

#endif
// File_checked_for_headers
