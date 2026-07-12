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
