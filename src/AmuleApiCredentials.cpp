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

#include "AmuleApiCredentials.h"

#include "Preferences.h"

#include <Credentials.h>

#include <wx/string.h>

#include <string>

namespace
{

// amuleapi is started with --config-dir=thePrefs::GetConfigDir(), so both
// processes resolve amuleapi-passwords to the same path.
std::string ConfigDir()
{
	return std::string(thePrefs::GetConfigDir().utf8_str());
}

} // namespace

namespace AmuleApiCredentials
{

bool ApplyPrefs(wxString &error)
{
	error.clear();

	// Straight translation: the preferences say what the user asked for,
	// webcommon knows what that means for the stored records. The rules
	// are not repeated here because the amuleapi CLI and REST endpoint
	// have to follow exactly the same ones.
	webcommon::CredentialChange change;
	change.admin_md5 = std::string(thePrefs::GetAmuleApiPass().Lower().utf8_str());
	change.guest_enabled = thePrefs::GetAmuleApiGuestIsEnabled();
	change.guest_md5 = std::string(thePrefs::GetAmuleApiGuestPass().Lower().utf8_str());

	std::string err;
	if (!webcommon::ApplyCredentialChange(ConfigDir(), change, err)) {
		error = wxString::FromUTF8(err.c_str());
		return false;
	}

	// Consumed. A pending password is a request, and replaying it on
	// every later save would keep re-hashing it and keep it in memory for
	// the rest of the session.
	thePrefs::SetAmuleApiPass(wxEmptyString);
	thePrefs::SetAmuleApiGuestPass(wxEmptyString);

	RefreshState();
	return true;
}

void RefreshState()
{
	webcommon::Credentials creds;
	std::string err;
	if (!webcommon::LoadCredentialsFile(ConfigDir(), creds, err)) {
		// A corrupt file is amuleapi's problem to report — it refuses to
		// start and names the bad key. Here it only means we cannot say
		// what is configured, so claim nothing rather than guess.
		thePrefs::SetAmuleApiAdminIsSet(false);
		thePrefs::SetAmuleApiGuestIsEnabled(false);
		return;
	}
	thePrefs::SetAmuleApiAdminIsSet(!creds.admin.empty());
	thePrefs::SetAmuleApiGuestIsEnabled(!creds.guest.empty());
}

} // namespace AmuleApiCredentials
