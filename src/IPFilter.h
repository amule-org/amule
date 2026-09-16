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
#include <wx/thread.h>
#include <algorithm>
#include <array>
#include <string>
#include <set>
#include <vector>

#include "Types.h" // Needed for uint8, uint16 and uint32
#include "NetworkAddress.h"
#include "PeerAddressing.h"

/** Sorted, disjoint native IPv6 ranges, resolved during loading.
 * Later entries override earlier ones, including above-threshold exemptions.
 */
class CIPFilterIPv6Ranges
{
public:
	bool Add(const CNetworkAddress &address, unsigned bits, unsigned level)
	{
		if (!Append(address, bits, level)) {
			return false;
		}
		Resolve();
		return true;
	}

private:
	friend class CIPFilterTask;

	// Loading appends all files in precedence order before resolving once.
	bool Append(const CNetworkAddress &address, unsigned bits, unsigned level)
	{
		if (!address.IsIPv6() || address.IsIPv4Mapped() || address.GetScopeId() != 0 || bits > 128 ||
			level > 255) {
			return false;
		}
		const auto network = address.TruncatedToPrefix(bits);
		auto last = network.GetOctets();
		for (unsigned bit = bits; bit < 128; ++bit) {
			last[bit / 8] |= static_cast<uint8>(0x80u >> (bit % 8));
		}
		m_rules.push_back({ network.GetOctets(), last, network, bits, level });
		++m_levelCounts[level];
		return true;
	}

	void Resolve()
	{
		struct Endpoint
		{
			CNetworkAddress::Octets address;
			std::size_t rule;
			bool start;
		};
		CNetworkAddress::Octets maximum;
		maximum.fill(255);
		std::vector<Endpoint> endpoints;
		endpoints.reserve(m_rules.size() * 2);
		for (std::size_t i = 0; i < m_rules.size(); ++i) {
			endpoints.push_back({ m_rules[i].first, i, true });
			if (m_rules[i].last != maximum) {
				endpoints.push_back({ Next(m_rules[i].last), i, false });
			}
		}
		std::sort(
			endpoints.begin(), endpoints.end(), [](const Endpoint &left, const Endpoint &right) {
				return left.address < right.address;
			});
		std::set<std::size_t> active;
		std::vector<Rule> resolved;
		// Sweep grouped endpoints in O(n log n); the newest active rule wins.
		for (std::size_t i = 0; i < endpoints.size();) {
			const auto first = endpoints[i].address;
			do {
				const auto &endpoint = endpoints[i++];
				if (endpoint.start) {
					active.insert(endpoint.rule);
				} else {
					active.erase(endpoint.rule);
				}
			} while (i < endpoints.size() && endpoints[i].address == first);
			if (!active.empty()) {
				auto rule = m_rules[*active.rbegin()];
				rule.first = first;
				rule.last = i < endpoints.size() ? Previous(endpoints[i].address) : maximum;
				resolved.push_back(rule);
			}
		}
		m_rules.swap(resolved);
	}

public:
	bool IsFiltered(const CNetworkAddress &address, unsigned level) const
	{
		if (!address.IsIPv6() || address.IsIPv4Mapped()) {
			return false;
		}
		auto it = std::upper_bound(m_rules.begin(),
			m_rules.end(),
			address.GetOctets(),
			[](const CNetworkAddress::Octets &key, const Rule &rule) {
				return key < rule.first;
			});
		if (it == m_rules.begin()) {
			return false;
		}
		--it;
		return address.GetOctets() <= it->last && it->level < level &&
		       PeerAddressing::MatchesFilterPrefix(address, it->network, it->bits);
	}

	unsigned BanCount(unsigned level) const
	{
		unsigned count = 0;
		for (unsigned i = 0; i < std::min(level, 256u); ++i) {
			count += m_levelCounts[i];
		}
		return count;
	}

private:
	// Only called when a surviving fragment proves there is no endpoint overflow.
	static CNetworkAddress::Octets Previous(CNetworkAddress::Octets bytes)
	{
		for (std::size_t i = bytes.size(); i > 0; --i) {
			if (bytes[i - 1]-- != 0) {
				break;
			}
		}
		return bytes;
	}

	static CNetworkAddress::Octets Next(CNetworkAddress::Octets bytes)
	{
		for (std::size_t i = bytes.size(); i > 0; --i) {
			if (++bytes[i - 1] != 0) {
				break;
			}
		}
		return bytes;
	}

	struct Rule
	{
		CNetworkAddress::Octets first;
		CNetworkAddress::Octets last;
		CNetworkAddress network;
		unsigned bits;
		unsigned level;
	};
	std::vector<Rule> m_rules;
	// Keep the public count of input rules independent of range splitting.
	std::array<unsigned, 256> m_levelCounts{};
};

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

	/** Mapped IPv4 uses the legacy path; absent addresses are rejected. */
	bool IsFiltered(const CNetworkAddress &address, bool isServer = false);

	/**
	 * The number of stored IPv4 ranges plus below-threshold IPv6 rules.
	 * Overlapping IPv6 rules are counted individually.
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
	CIPFilterIPv6Ranges m_ipv6Ranges;
	unsigned m_ipv6AccessLevel = 0;

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
