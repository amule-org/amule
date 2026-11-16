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

#ifndef EC_CLIENT_PASSWORD_H
#define EC_CLIENT_PASSWORD_H

#include "Types.h"
#include <wx/config.h>
#include <wx/intl.h>
#include <wx/log.h>
#include <common/Format.h>
#include <common/MD5Sum.h>

namespace ECClientPassword
{
// The only password key of a client config: remote.conf and amuleapi.conf.
constexpr char KEY[] = "/EC/PasswordMD5";
// The key it replaces.
constexpr char LEGACY_KEY[] = "/EC/Password";

// What the replaced [EC]/Password held.
enum class LegacyPassword
{
	Digest,
	Plaintext
};

// Moves [EC]/Password to KEY. `changed` says the caller must save. Two nonempty keys fail, since
// neither can be known to be the current one.
inline bool MigrateLegacyKey(wxConfigBase &cfg, LegacyPassword legacy, bool &changed, wxString &error)
{
	changed = false;
	error.clear();
	if (!cfg.HasEntry(LEGACY_KEY)) {
		return true;
	}
	const wxString old = cfg.Read(LEGACY_KEY, wxString());
	const wxString current = cfg.Read(KEY, wxString());
	if (!old.IsEmpty() && !current.IsEmpty()) {
		error = _("[EC]/Password and [EC]/PasswordMD5 cannot both be nonempty; remove one");
		return false;
	}
	if (!old.IsEmpty()) {
		cfg.Write(KEY, legacy == LegacyPassword::Plaintext ? MD5Sum(old).GetHash() : old);
	}
	cfg.DeleteEntry(LEGACY_KEY);
	changed = true;
	return true;
}

// The caller explains the failure; wx's own lines would only repeat it.
inline bool FlushQuietly(wxConfigBase &cfg)
{
	wxLogNull quiet;
	return cfg.Flush();
}

enum class Migration
{
	Unchanged,
	Saved,
	Unsaved,
	Conflict
};

// remote.conf, shared by amulecmd and amulegui. Unsaved: the caller uses the migrated key for
// this run only.
inline Migration MigrateRemoteConf(wxConfigBase &cfg, wxString &error)
{
	bool changed = false;
	if (!MigrateLegacyKey(cfg, LegacyPassword::Digest, changed, error)) {
		return Migration::Conflict;
	}
	if (!changed) {
		return Migration::Unchanged;
	}
	return FlushQuietly(cfg) ? Migration::Saved : Migration::Unsaved;
}

inline wxString RemoteConfConflictMessage(const wxString &path, const wxString &error)
{
	return CFormat("%s: %s. %s") % path % error % _("To replace both, run amulecmd -w --password-stdin.");
}

inline wxString RemoteConfUnsavedMessage(const wxString &path)
{
	return CFormat(_("Cannot save %s; using its migrated password for this run only.")) % path;
}

inline wxString Display(const wxString &pending)
{
	return pending.IsEmpty() ? wxString() : wxString("********");
}

// Preserve a pending digest while the dialog remains open. Only newly entered
// text is a password; the displayed mask must never be hashed as one. Closing
// the dialog discards the pending edit.
inline bool ReadInput(wxString &pending, const wxString &text, bool modified)
{
	if (modified || text != Display(pending)) {
		pending = text.IsEmpty() ? wxString() : MD5Sum(text).GetHash();
		return true;
	}
	return !pending.IsEmpty() && pending != "********";
}

// The server has already committed next. Always update the in-memory reconnect
// credential, but only replace a saved credential for this exact connection.
// In particular, rotating a password must not opt a user into saving passwords.
inline bool Commit(
	wxConfigBase &cfg, const wxString &host, int port, wxString &sessionPassword, const wxString &next)
{
	const bool remembered = cfg.Read("/EC/Host", wxString()) == host &&
				cfg.Read("/EC/Port", 4712L) == port && !sessionPassword.IsEmpty() &&
				cfg.Read(KEY, wxString()) == sessionPassword;
	sessionPassword = next;
	return !remembered || (cfg.Write(KEY, next) && cfg.Flush());
}
} // namespace ECClientPassword

#endif
