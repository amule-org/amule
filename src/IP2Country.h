//
// This file is part of the aMule Project.
//
// Copyright (c) 2004-2011 Marcelo Roberto Jimenez ( phoenix@amule.org )
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

//
// Country flags are from FAMFAMFAM (http://www.famfamfam.com)
//
// Flag icons - http://www.famfamfam.com
//
// These icons are public domain, and as such are free for any use (attribution appreciated but not required).
//
// Note that these flags are named using the ISO3166-1 alpha-2 country codes where appropriate.
// A list of codes can be found at http://en.wikipedia.org/wiki/ISO_3166-1_alpha-2
//
// If you find these icons useful, please donate via paypal to mjames@gmail.com
// (or click the donate button available at http://www.famfamfam.com/lab/icons/silk)
//
// Contact: mjames@gmail.com
//

#ifndef IP2COUNTRY_H
#define IP2COUNTRY_H

#include "Types.h" // Needed for uint8, uint16 and uint32

#include <functional>
#include <unordered_map>

#include <wx/string.h>

class CMaxMindDBDatabase;

// Headless GeoIP resolver. Maps an IP to an ISO 3166-1 alpha-2 country code
// via the on-disk MaxMind DB and manages downloading/updating that DB from the
// user-configured source (thePrefs). Deliberately free of any GUI dependency
// (no wxImage / flags / prefs-dialog) so it can live in the core (CamuleApp)
// and run headless in amuled — the flag presentation is a separate GUI concern
// (see CCountryFlags), and manual-update failure popups are delegated to the
// GUI via SetUpdateFailedNotifier().
class CIP2Country
{
public:
	CIP2Country(const wxString &configDir);
	~CIP2Country();

	// Resolve an IP to its ISO 3166-1 alpha-2 country code (lowercase).
	// Empty when GeoIP is disabled, unsupported by the build, or the IP
	// does not resolve.
	wxString GetCountryCode(const wxString &ip);

	// Same, from the numeric IP the callers already hold, and memoised.
	//
	// The returned reference is valid until the next call on this object, or
	// any Enable()/Disable(): the cache is cleared wholesale on overflow and
	// on either of those, and clear() invalidates references. Copy it if you
	// need it to outlive the call -- in particular do not pass two of these
	// into one expression.
	//
	// #439 moved this resolution from GUI paint into the core. Paint hid the
	// cost: it ran only for visible rows, only while the list was on screen,
	// only with someone watching. The EC client tag now resolves EVERY peer
	// on EVERY poll -- roughly every 3 s, headless, forever -- and a peer's
	// country cannot change while its IP does not. Measured at ~7.5us per
	// peer per poll on a live daemon, about 37% of the client tag build.
	//
	// Keyed on the numeric IP so the caller need not format a string first
	// (Uint32toStringIP allocates); the string is only built on a miss.
	const wxString &GetCountryCode(uint32 ip);

	void Enable();
	void Disable();
	// Refresh the on-disk MMDB from the configured source.
	// manualUpdate=true is set by the prefs "Update now" button so that
	// failures (no credential, bad URL, HTTP error) surface via the
	// update-failed notifier (a GUI popup) in addition to the network log;
	// auto-update (startup) stays silent so users don't get a popup every
	// cold boot if their chosen source is briefly down.
	// showProgress=true renders the HTTP progress dialog — appropriate for a
	// LOCAL monolithic "Update now". It is false for a REMOTE (amulegui, over
	// EC) trigger: EC carries no download progress, so the dialog would be
	// blank/unhelpful, and on a monolithic-app-as-backend it would pop on the
	// core rather than the remote GUI. (No-op on a headless daemon either way.)
	void Update(bool manualUpdate = false, bool showProgress = true);
	bool IsEnabled();
	void DownloadFinished(uint32 result);

	// Path of the on-disk MMDB file. Exposed so the IP2Country
	// preferences panel can show the status line ("Loaded — <path>"),
	// without re-deriving the config-dir + filename convention.
	const wxString &GetDatabasePath() const { return m_DataBasePath; }
	// Live status for the prefs panel (local, and carried to amulegui over EC,
	// #440 remote config). IsDownloading() is true while a refresh is in
	// flight; GetLastResult() is a short human string describing the outcome
	// of the last completed update (empty until the first one runs).
	bool IsDownloading() const { return m_downloading; }

	const wxString &GetLastResult() const { return m_lastResult; }

	// Optional hook so a GUI front-end can surface a *manual* update
	// failure (the "Update now" button) as a popup. Left unset in headless
	// builds (amuled), where the failure only reaches the network log.
	void SetUpdateFailedNotifier(std::function<void(const wxString &)> notifier)
	{
		m_updateFailedNotifier = std::move(notifier);
	}

private:
	// Drop every memoised resolution. Called wherever the answer could change
	// underneath the cache: enable and disable, which between them also cover a
	// completed database refresh (DownloadFinished routes through both).
	// Without this a headless daemon is the worst case -- entries resolved
	// while GeoIP was off would stay empty forever, with no repaint to force a
	// re-read and nobody watching to notice.
	void InvalidateCountryCache();

	// Numeric IP -> ISO code. Bounded: peers churn, so an unbounded map on a
	// long-lived daemon would grow without limit. On overflow the whole map is
	// dropped rather than evicting cleverly -- a rebuild costs one lookup per
	// live peer, which is what a single poll cost before this existed.
	//
	// Not synchronised. Like the rest of this class it is touched from the main
	// thread only; a concurrent reader during a clear() would be a race.
	std::unordered_map<uint32, wxString> m_countryCache;
	static const size_t kMaxCountryCacheEntries = 8192;

	CMaxMindDBDatabase *m_db;
	wxString m_DataBaseName;
	wxString m_DataBasePath;

	// DB-IP fallback retry tracking. The first attempt fetches the
	// current month's URL; if that fails (commonly a 404 in the first
	// few days of a month before DB-IP publishes the new dataset), the
	// download callback retries with monthOffset=-1. Reset to false on
	// every Update() entry.
	bool m_TriedPreviousMonth;

	// Set by Update(true) (the "Update now" button) so the failure
	// paths in StartDownload + DownloadFinished know to surface a
	// popup, not just a log line.
	bool m_ManualUpdate;

	// GUI-supplied popup hook for manual-update failures; unset = headless.
	std::function<void(const wxString &)> m_updateFailedNotifier;

	// Live status (see IsDownloading / GetLastResult).
	bool m_downloading = false;
	wxString m_lastResult;

	// Whether the in-flight download should render an HTTP progress dialog.
	// Carried across the DB-IP previous-month retry in StartDownload. See Update().
	bool m_showProgress = true;

	void StartDownload(int monthOffset);
	void NotifyUpdateFailed(const wxString &msg);
};

#endif // IP2COUNTRY_H
