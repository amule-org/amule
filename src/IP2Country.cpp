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

#include "config.h" // Needed for ENABLE_IP2COUNTRY

#ifdef ENABLE_IP2COUNTRY

#include "Preferences.h" // For thePrefs
#include "CFile.h"       // For CPath
#include "HTTPDownload.h"
#include "Logger.h"               // For AddLogLineM()
#include <common/Format.h>        // For CFormat()
#include "common/FileFunctions.h" // For UnpackArchive

#include <wx/intl.h>

#include "IP2Country.h"
#include "geoip/MaxMindDBDatabase.h"

CIP2Country::CIP2Country(const wxString &configDir)
: m_db(new CMaxMindDBDatabase())
, m_TriedPreviousMonth(false)
, m_ManualUpdate(false)
{
	m_DataBaseName = "geoip.mmdb";
	m_DataBasePath = configDir + m_DataBaseName;

	// One-shot migration: the v2.x file lived at GeoLite2-Country.mmdb.
	// If that legacy file exists and the new canonical geoip.mmdb does
	// not, move it across so an upgrading user doesn't lose flag display
	// silently. If both exist (e.g. they followed the new docs while
	// keeping the old file around) leave each alone.
	const wxString legacyPath = configDir + "GeoLite2-Country.mmdb";
	if (CPath::FileExists(legacyPath) && !CPath::FileExists(m_DataBasePath)) {
		if (wxRenameFile(legacyPath, m_DataBasePath)) {
			AddLogLineN(
				CFormat(_("Migrated existing GeoLite2-Country.mmdb to %s")) % m_DataBasePath);
		}
	}
}

bool CIP2Country::IsEnabled()
{
	return m_db && m_db->IsOpen();
}

void CIP2Country::Enable()
{
	Disable();

	if (!CPath::FileExists(m_DataBasePath)) {
		Update();
		return;
	}

	m_db->Open(m_DataBasePath);

	// One-shot backfill: files written by builds older than the
	// source-aware prefs have no LoadedSource recorded, which would
	// leave the prefs status line attribution-less. Best-effort guess:
	// attribute the existing file to the currently configured source
	// so the status line shows *something* meaningful. The user can
	// always click "Update now" to overwrite with the real source.
	if (m_db->IsOpen() && thePrefs::GetGeoIPLoadedSource().IsEmpty()) {
		thePrefs::SetGeoIPLoadedSource(thePrefs::GetGeoIPSource());
	}
}

void CIP2Country::Update(bool manualUpdate, bool showProgress)
{
	m_TriedPreviousMonth = false;
	m_ManualUpdate = manualUpdate;
	m_showProgress = showProgress;
	StartDownload(0);
}

void CIP2Country::StartDownload(int monthOffset)
{
	const wxString url = thePrefs::GetGeoIPResolvedDownloadUrl(monthOffset);
	if (url.IsEmpty()) {
		wxString msg;
		switch (thePrefs::GetGeoIPSource()) {
		case CPreferences::GeoIPSourceMaxMind:
			msg = _("IP2Country: MaxMind selected as the GeoIP source but no License Key "
				"configured. Open Preferences → IP2Country, paste your free MaxMind "
				"License Key and click 'Update now'.");
			break;
		case CPreferences::GeoIPSourceCustom:
			msg = _("IP2Country: Custom URL selected as the GeoIP source but no URL "
				"configured. Open Preferences → IP2Country and supply a URL that "
				"points to an .mmdb (or .gz / .tar.gz containing one).");
			break;
		default:
			msg = _("IP2Country: failed to resolve a GeoIP download URL.");
			break;
		}
		AddLogLineC(msg);
		if (m_ManualUpdate) {
			NotifyUpdateFailed(msg);
		}
		m_ManualUpdate = false;
		m_downloading = false;
		m_lastResult = msg;
		thePrefs::SetGeoIPEnabled(false);
		return;
	}
	AddLogLineN(CFormat(_("Download new %s from %s")) % m_DataBaseName % url);
	m_downloading = true;
	// showDialog = m_showProgress: shown for a local monolithic "Update now",
	// suppressed for a remote (amulegui/EC) trigger — EC carries no progress and
	// on a monolithic-app-as-backend it would pop on the core (#440). No-op on a
	// headless daemon. checkDownloadNewer stays true (honour If-Modified).
	CHTTPDownloadThread *downloader = new CHTTPDownloadThread(
		url, m_DataBasePath + ".download", m_DataBasePath, HTTP_GeoIP, m_showProgress, true);
	downloader->Create();
	downloader->Run();
}

void CIP2Country::Disable()
{
	if (m_db) {
		m_db->Close();
	}
}

void CIP2Country::DownloadFinished(uint32 result)
{
	// Snapshot + clear the manual flag up front so any early return
	// below doesn't leave it set for the next StartDownload (e.g. a
	// subsequent auto-update would inherit the popup behaviour).
	const bool manual = m_ManualUpdate;
	m_ManualUpdate = false;
	// The download finished; the DB-IP early-month retry below re-arms this.
	m_downloading = false;

	if (result == HTTP_Success) {
		Disable();
		// download succeeded. Switch over to new database.
		wxString newDat = m_DataBasePath + ".download";

		// Try to unpack the file, might be an archive
		wxScopedCharBuffer dataBaseName = m_DataBaseName.utf8_str();
		const char *geoip_files[] = { dataBaseName, NULL };

		if (UnpackArchive(CPath(newDat), geoip_files).second == EFT_Error) {
			const wxString msg =
				CFormat(_("Download of %s file failed, aborting update.")) % m_DataBaseName;
			AddLogLineC(msg);
			if (manual) {
				NotifyUpdateFailed(msg);
			}
			return;
		}

		if (wxFileExists(m_DataBasePath)) {
			if (!wxRemoveFile(m_DataBasePath)) {
				const wxString msg =
					CFormat(_("Failed to remove %s file, aborting update.")) %
					m_DataBaseName;
				AddLogLineC(msg);
				if (manual) {
					NotifyUpdateFailed(msg);
				}
				return;
			}
		}

		if (!wxRenameFile(newDat, m_DataBasePath)) {
			const wxString msg =
				CFormat(_("Failed to rename %s file, aborting update.")) % m_DataBaseName;
			AddLogLineC(msg);
			if (manual) {
				NotifyUpdateFailed(msg);
			}
			return;
		}

		Enable();
		if (IsEnabled()) {
			const wxString msg = CFormat(_("Successfully updated %s")) % m_DataBaseName;
			AddLogLineN(msg);
			m_lastResult = msg;
			// Record which source actually wrote the file so the prefs
			// status line can attribute it correctly even after the
			// user flips the source dropdown to a different provider
			// they haven't downloaded from yet.
			thePrefs::SetGeoIPLoadedSource(thePrefs::GetGeoIPSource());
		} else {
			const wxString msg = CFormat(_("Error updating %s")) % m_DataBaseName;
			AddLogLineC(msg);
			m_lastResult = msg;
			if (manual) {
				NotifyUpdateFailed(msg);
			}
		}
	} else if (result == HTTP_Skipped) {
		const wxString msg =
			CFormat(_("Skipped download of %s, because requested file is not newer.")) %
			m_DataBaseName;
		AddLogLineN(msg);
		m_lastResult = msg;
	} else {
		// DB-IP early-month fallback: the new month's file frequently
		// 404s for the first few days while DB-IP publishes it. Retry
		// once with monthOffset=-1 so the previous (definitely-published)
		// month carries the user through the gap. MaxMind / Custom URLs
		// aren't month-templated, so the fallback is gated on source.
		// Re-arm the manual flag so the retry's eventual outcome still
		// surfaces a popup; we only cleared it as a one-shot guard.
		if (thePrefs::GetGeoIPSource() == CPreferences::GeoIPSourceDBIP && !m_TriedPreviousMonth) {
			m_TriedPreviousMonth = true;
			m_ManualUpdate = manual;
			AddLogLineN(_("DB-IP download failed for the current month - retrying with "
				      "the previous month's URL."));
			StartDownload(-1);
			return;
		}
		const wxString msg = CFormat(_("Failed to download %s from %s")) % m_DataBaseName %
				     thePrefs::GetGeoIPResolvedDownloadUrl(m_TriedPreviousMonth ? -1 : 0);
		AddLogLineC(msg);
		m_lastResult = msg;
		if (manual) {
			NotifyUpdateFailed(msg);
		}
		// if it failed and there is no database, turn it off
		if (!wxFileExists(m_DataBasePath)) {
			thePrefs::SetGeoIPEnabled(false);
		}
	}
}

CIP2Country::~CIP2Country()
{
	Disable();
	delete m_db;
}

wxString CIP2Country::GetCountryCode(const wxString &ip)
{
	if (!IsEnabled()) {
		return wxEmptyString;
	}
	return m_db->GetCountryCode(ip);
}

void CIP2Country::NotifyUpdateFailed(const wxString &msg)
{
	if (m_updateFailedNotifier) {
		m_updateFailedNotifier(msg);
	}
}

#else

#include "IP2Country.h"

CIP2Country::CIP2Country(const wxString &)
{
	m_db = NULL;
}

CIP2Country::~CIP2Country() {}
void CIP2Country::Enable() {}
void CIP2Country::Disable() {}
void CIP2Country::Update(bool, bool) {}
void CIP2Country::DownloadFinished(uint32) {}
void CIP2Country::StartDownload(int) {}
void CIP2Country::NotifyUpdateFailed(const wxString &) {}
bool CIP2Country::IsEnabled()
{
	return false;
}

wxString CIP2Country::GetCountryCode(const wxString &)
{
	return wxEmptyString;
}

#endif // ENABLE_IP2COUNTRY
