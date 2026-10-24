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

#include "ECSRPConfig.h"
#include "ECSRP.h"
#include <wx/intl.h>
#include <stdexcept>

namespace ECSRP
{
namespace
{
const char *const KEY = "/ExternalConnect/ECPasswordVerifier";
const char *const LEGACY = "/ExternalConnect/ECPassword";
wxString CreateRecord(const wxString &digest)
{
	if (!digest.IsEmpty() && !IsPasswordDigest(std::string(digest.utf8_str())))
		throw std::invalid_argument("Invalid EC password digest");
	return digest.IsEmpty() ? wxString()
				: wxString::FromUTF8(
					  Verifier::Create(std::string(digest.utf8_str())).Encode().c_str());
}
void Cleanup(wxConfigBase &cfg, wxString &error)
{
	// Never restore the old digest in memory: a subsequent password rotation
	// must not leave a stale legacy key that wins on the next startup.
	if (cfg.HasEntry(LEGACY) && (!cfg.DeleteEntry(LEGACY) || !cfg.Flush()))
		error = _("Could not remove legacy ECPassword; cleanup will be retried on restart.");
}
bool Persist(wxConfigBase &cfg, const wxString &next)
{
	const bool existed = cfg.HasEntry(KEY);
	const wxString previous = cfg.Read(KEY, wxString());
	if (cfg.Write(KEY, next) && cfg.Flush())
		return true;
	// wxFileConfig may flush on exit. Restore the old persisted state too.
	if (existed)
		cfg.Write(KEY, previous);
	else
		cfg.DeleteEntry(KEY);
	return false;
}
bool Migrate(wxConfigBase &cfg, const wxString &legacy, wxString &verifier, wxString &error)
{
	try {
		const wxString next = CreateRecord(legacy);
		if (Persist(cfg, next)) {
			verifier = next;
			Cleanup(cfg, error);
		} else {
			verifier = next;
			error = _("Could not persist ECPasswordVerifier; using the legacy ECPassword in "
				  "memory for this run. Migration will be retried on restart.");
		}
		return true;
	} catch (const std::exception &) {
		verifier.clear();
		error = _("Could not generate ECPasswordVerifier; external connections are disabled.");
		return false;
	}
}
} // namespace
bool StoreCredential(wxConfigBase &cfg, const wxString &digest, wxString &verifier, wxString &error)
{
	error.clear();
	try {
		const wxString next = CreateRecord(digest);
		// Unlike migration, an explicit rotation must never persist a new
		// verifier alongside a stale legacy password: startup would interpret
		// that legacy key as a later password edit made by a downgraded core.
		const bool hadLegacy = cfg.HasEntry(LEGACY);
		const wxString legacy = cfg.Read(LEGACY, wxString());
		if (hadLegacy && !cfg.DeleteEntry(LEGACY)) {
			error = _("EC password change rejected: could not persist ECPasswordVerifier. The "
				  "active credential is unchanged.");
			return false;
		}
		if (!Persist(cfg, next)) {
			if (hadLegacy)
				cfg.Write(LEGACY, legacy);
			error = _("EC password change rejected: could not persist ECPasswordVerifier. The "
				  "active credential is unchanged.");
			return false;
		}
		verifier = next;
		return true;
	} catch (const std::exception &) {
		error = _("EC password change rejected: could not generate ECPasswordVerifier. The active "
			  "credential is unchanged.");
		return false;
	}
}
bool LoadCredential(wxConfigBase &cfg, wxString &verifier, wxString &error)
{
	error.clear();
	verifier.clear();
	const wxString legacy = cfg.Read(LEGACY, wxString());
	if (!legacy.IsEmpty() && !IsPasswordDigest(std::string(legacy.utf8_str()))) {
		error = _("ECPassword contains an invalid or empty-password digest. Run amuled "
			  "--ec-config to set a new EC password.");
		return false;
	}
	if (cfg.HasEntry(KEY)) {
		verifier = cfg.Read(KEY, wxString());
		try {
			if (!verifier.IsEmpty())
				Verifier::Decode(std::string(verifier.utf8_str()));
		} catch (const std::exception &) {
			error = _(
				"Invalid or unsupported ECPasswordVerifier; refusing external connections.");
			return false;
		}
		if (cfg.HasEntry(LEGACY)) {
			// A downgraded core recreates an empty key on any preferences save.
			// Only a non-empty digest can carry a newer password.
			bool matches = legacy.IsEmpty();
			try {
				if (!verifier.IsEmpty() && !legacy.IsEmpty())
					matches = Verifier::Decode(std::string(verifier.utf8_str()))
							  .Matches(std::string(legacy.utf8_str()));
			} catch (const std::exception &) {
				error = _("Could not compare legacy ECPassword with ECPasswordVerifier; "
					  "refusing external connections.");
				return false;
			}
			if (!matches) {
				const bool ok = Migrate(cfg, legacy, verifier, error);
				if (ok)
					error = _("Migrating a changed legacy ECPassword after downgrade; "
						  "the previous verifier no longer authenticates this run.") +
						" " + error;
				return ok;
			}
		}
		Cleanup(cfg, error);
		return true;
	}
	return legacy.IsEmpty() || Migrate(cfg, legacy, verifier, error);
}
} // namespace ECSRP
