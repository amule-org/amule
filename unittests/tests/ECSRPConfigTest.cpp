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

#include "config.h"
#include "ECSRP.h"
#include "ECSRPConfig.h"
#include "ECClientPassword.h"
#include <wx/fileconf.h>
#include <iostream>
#include <stdexcept>

using namespace ECSRP;
static void Check(bool ok, const char *message)
{
	if (!ok)
		throw std::runtime_error(message);
}
class FaultConfig : public wxFileConfig
{
public:
	FaultConfig()
	: wxFileConfig("", "", "", "", 0)
	{
	}
	int calls = 0, failAt = 0;
	bool failDelete = false;
	bool DeleteEntry(const wxString &key, bool deleteGroupIfEmpty = true) override
	{
		return !failDelete && wxFileConfig::DeleteEntry(key, deleteGroupIfEmpty);
	}
	wxString savedVerifier, savedLegacy;
	bool savedHasVerifier = false;
	bool Flush(bool = false) override
	{
		if (++calls == failAt)
			return false;
		savedHasVerifier = HasEntry("/ExternalConnect/ECPasswordVerifier");
		savedVerifier = Read("/ExternalConnect/ECPasswordVerifier", wxString());
		savedLegacy = Read("/ExternalConnect/ECPassword", wxString());
		return true;
	}
};

int main()
{
	try {
		// Confirmed remote rotations update reconnect state while respecting the
		// user's save-password choice and credentials saved for other endpoints.
		for (int scenario = 0; scenario < 6; ++scenario) {
			FaultConfig cfg;
			cfg.Write("/EC/Host", scenario == 1 ? "other-host" : "localhost");
			cfg.Write("/EC/Port", scenario == 2 ? 4713L : 4712L);
			if (scenario != 3)
				cfg.Write("/EC/Password", scenario == 5 ? "unrelated" : "old");
			if (scenario == 4)
				cfg.failAt = 1;
			wxString session = "old";
			Check(ECClientPassword::Commit(cfg, "localhost", 4712, session, "new") ==
					(scenario != 4),
				"saved client credential failure was hidden");
			Check(session == "new", "reconnect kept the old password after rotation");
			Check(cfg.calls == ((scenario == 0 || scenario == 4) ? 1 : 0),
				"rotation changed an unrelated or unsaved credential");
			if (scenario == 1 || scenario == 2)
				Check(cfg.Read("/EC/Password", wxString()) == "old",
					"rotation overwrote another endpoint's password");
			if (scenario == 3)
				Check(!cfg.HasEntry("/EC/Password"), "rotation enabled password saving");
			if (scenario == 0)
				Check(cfg.Read("/EC/Password", wxString()) == "new",
					"remembered password was not updated");
			if (scenario == 5)
				Check(cfg.Read("/EC/Password", wxString()) == "unrelated",
					"rotation overwrote a separately saved credential");
		}
		for (const auto &digest : { std::string(),
			     std::string(32, '0'),
			     std::string("d41d8cd98f00b204e9800998ecf8427e"),
			     std::string("D41D8CD98F00B204E9800998ECF8427E"),
			     std::string(32, 'z') }) {
			Check(!IsPasswordDigest(digest), "unusable password digest accepted");
			if (!digest.empty()) {
				FaultConfig cfg;
				wxString existing = "unchanged", error;
				Check(!StoreCredential(
					      cfg, wxString::FromUTF8(digest.c_str()), existing, error),
					"unusable password committed");
				Check(existing == "unchanged" && cfg.calls == 0,
					"invalid password changed credential state");
			}
		}
		const std::string password = "0123456789abcdef0123456789abcdef";
		const auto verifier = Verifier::Create(password);
		// Migration must reject credentials that production clients cannot use,
		// including a downgrade that replaces an existing verifier with one.
		for (const auto &legacy : { wxString(32, '0'),
			     wxString("d41d8cd98f00b204e9800998ecf8427e"),
			     wxString("D41D8CD98F00B204E9800998ECF8427E"),
			     wxString(32, 'z') }) {
			for (bool existing : { false, true }) {
				FaultConfig cfg;
				const wxString original = wxString::FromUTF8(verifier.Encode().c_str());
				if (existing)
					cfg.Write("/ExternalConnect/ECPasswordVerifier", original);
				cfg.Write("/ExternalConnect/ECPassword", legacy);
				wxString active, warning;
				Check(!LoadCredential(cfg, active, warning),
					"unusable legacy credential migrated");
				Check(warning.Contains("ECPassword contains") &&
						warning.Contains("--ec-config") &&
						!warning.Contains("Could not generate"),
					"invalid legacy digest did not name the setting and recovery "
					"command");
				Check(cfg.calls == 0 &&
						cfg.Read("/ExternalConnect/ECPassword", wxString()) ==
							legacy &&
						cfg.HasEntry("/ExternalConnect/ECPasswordVerifier") ==
							existing &&
						(!existing || cfg.Read("/ExternalConnect/ECPasswordVerifier",
								      wxString()) == original),
					"rejected migration modified configuration");
			}
		}
		for (int failAt : { 0, 1, 2 }) {
			FaultConfig cfg;
			cfg.Write("/ExternalConnect/ECPassword", wxString::FromUTF8(password.c_str()));
			cfg.failAt = failAt;
			wxString record, error;
			const bool loaded = LoadCredential(cfg, record, error);
			Check(loaded, "migration unavailable after persistence failure");
			if (failAt == 1) {
				Check(!record.IsEmpty() &&
						!cfg.HasEntry("/ExternalConnect/ECPasswordVerifier"),
					"failed migration changed verifier");
				Check(cfg.Read("/ExternalConnect/ECPassword", wxString()) == password,
					"failed migration lost old credential");
				Check(Verifier::Decode(std::string(record.utf8_str())).Matches(password),
					"in-memory migration cannot authenticate the legacy password");
			} else {
				Check(Verifier::Decode(std::string(record.utf8_str())).Valid(),
					"persisted invalid verifier");
				Check(!cfg.savedVerifier.IsEmpty(), "cleanup before verifier commit");
				FaultConfig restarted;
				restarted.Write("/ExternalConnect/ECPasswordVerifier", cfg.savedVerifier);
				if (!cfg.savedLegacy.IsEmpty())
					restarted.Write("/ExternalConnect/ECPassword", cfg.savedLegacy);
				wxString again;
				Check(LoadCredential(restarted, again, error), "migration restart failed");
				Check(again == record, "migration regenerated verifier");
				Check(!restarted.HasEntry("/ExternalConnect/ECPassword"),
					"restart did not clean legacy key");
			}
		}
		// A 3.1 preferences save recreates ECPassword= after migration even
		// without a password edit. It must never erase/re-salt the verifier.
		for (int failAt : { 0, 1 }) {
			FaultConfig emptyLegacy;
			const wxString original = wxString::FromUTF8(verifier.Encode().c_str());
			emptyLegacy.Write("/ExternalConnect/ECPasswordVerifier", original);
			emptyLegacy.Write("/ExternalConnect/ECPassword", "");
			emptyLegacy.failAt = failAt;
			wxString retained, warning;
			Check(LoadCredential(emptyLegacy, retained, warning), "empty legacy key disabled EC");
			Check(retained == original && emptyLegacy.Read("/ExternalConnect/ECPasswordVerifier",
							      wxString()) == original,
				"empty legacy key replaced the existing verifier");
			Check(!emptyLegacy.HasEntry("/ExternalConnect/ECPassword"),
				"empty legacy key not cleaned in memory");
			Check(emptyLegacy.calls == 1, "empty legacy key triggered verifier persistence");
			Check((failAt == 1) == !warning.IsEmpty(), "cleanup warning missing or spurious");
		}
		FaultConfig downgrade;
		downgrade.Write(
			"/ExternalConnect/ECPasswordVerifier", wxString::FromUTF8(verifier.Encode().c_str()));
		const std::string newer = "abcdef0123456789abcdef0123456789";
		downgrade.Write("/ExternalConnect/ECPassword", wxString::FromUTF8(newer.c_str()));
		wxString migrated, migrationError;
		Check(LoadCredential(downgrade, migrated, migrationError), "re-upgrade migration failed");
		const auto replaced = Verifier::Decode(std::string(migrated.utf8_str()));
		Check(replaced.Matches(newer) && !replaced.Matches(password),
			"re-upgrade resurrected old password");
		Check(replaced.salt != verifier.salt && !migrationError.empty(),
			"downgrade migration not logged or salted");
		Check(!downgrade.HasEntry("/ExternalConnect/ECPassword"), "new legacy key not removed");
		FaultConfig cleanup;
		cleanup.Write("/ExternalConnect/ECPassword", wxString::FromUTF8(password.c_str()));
		cleanup.failAt = 2;
		Check(LoadCredential(cleanup, migrated, migrationError),
			"cleanup failure rejected committed verifier");
		Check(!cleanup.HasEntry("/ExternalConnect/ECPassword"),
			"failed cleanup restored stale legacy key in memory");
		Check(StoreCredential(cleanup, wxString::FromUTF8(newer.c_str()), migrated, migrationError),
			"rotation after failed cleanup failed");
		Check(LoadCredential(cleanup, migrated, migrationError) &&
				Verifier::Decode(std::string(migrated.utf8_str())).Matches(newer),
			"cleanup failure resurrected old password");
		// A rotation cannot expose an intermediate persisted state containing
		// the new verifier and old digest, even if a second flush would fail.
		for (int failure : { 0, 1, 2 }) {
			FaultConfig rotation;
			const wxString original = wxString::FromUTF8(verifier.Encode().c_str());
			rotation.Write("/ExternalConnect/ECPasswordVerifier", original);
			rotation.Write("/ExternalConnect/ECPassword", wxString::FromUTF8(password.c_str()));
			rotation.failAt = failure == 1 ? 1 : 2;
			rotation.failDelete = failure == 2;
			wxString active = original, warning;
			const bool ok =
				StoreCredential(rotation, wxString::FromUTF8(newer.c_str()), active, warning);
			Check(ok == (failure == 0), "rotation failure was hidden");
			if (ok) {
				Check(rotation.calls == 1 && rotation.savedLegacy.IsEmpty(),
					"rotation persisted an obsolete legacy password");
				FaultConfig restarted;
				restarted.Write(
					"/ExternalConnect/ECPasswordVerifier", rotation.savedVerifier);
				wxString again;
				Check(LoadCredential(restarted, again, warning) && again == active &&
						Verifier::Decode(std::string(again.utf8_str()))
							.Matches(newer),
					"rotation did not survive restart");
			} else {
				Check(active == original &&
						rotation.Read("/ExternalConnect/ECPasswordVerifier",
							wxString()) == original &&
						rotation.Read("/ExternalConnect/ECPassword", wxString()) ==
							password,
					"failed rotation changed the previous credential state");
			}
		}
		FaultConfig disabled;
		disabled.Write(
			"/ExternalConnect/ECPasswordVerifier", wxString::FromUTF8(verifier.Encode().c_str()));
		disabled.Write("/ExternalConnect/ECPassword", wxString::FromUTF8(password.c_str()));
		disabled.failAt = 2;
		Check(StoreCredential(disabled, "", migrated, migrationError) && migrated.IsEmpty() &&
				disabled.calls == 1 && disabled.savedHasVerifier &&
				disabled.savedVerifier.IsEmpty() && disabled.savedLegacy.IsEmpty(),
			"clearing the password left a legacy credential to resurrect");
		FaultConfig corrupt;
		corrupt.Write("/ExternalConnect/ECPassword", wxString::FromUTF8(password.c_str()));
		corrupt.Write("/ExternalConnect/ECPasswordVerifier", "v9:unsupported");
		wxString bad, error;
		Check(!LoadCredential(corrupt, bad, error),
			"malformed verifier fell back to legacy credential");
		Check(corrupt.calls == 0 && corrupt.HasEntry("/ExternalConnect/ECPassword"),
			"malformed verifier modified config");
		FaultConfig rekey;
		rekey.Write(
			"/ExternalConnect/ECPasswordVerifier", wxString::FromUTF8(verifier.Encode().c_str()));
		wxString existing = wxString::FromUTF8(verifier.Encode().c_str());
		rekey.failAt = 1;
		Check(!StoreCredential(rekey, "abcdef0123456789abcdef0123456789", existing, error),
			"failed rekey accepted");
		Check(std::string(existing.utf8_str()) == verifier.Encode(),
			"failed rekey changed live credential");
		Check(rekey.Read("/ExternalConnect/ECPasswordVerifier", wxString()) == existing,
			"failed rekey changed config");
		std::cout << "ECSRP credential config tests passed\n";
	} catch (const std::exception &e) {
		std::cerr << e.what() << '\n';
		return 1;
	}
}
