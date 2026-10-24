// This file is part of aMule. Licensed under GPL-2.0-or-later.
// Small GUI client for the fake-portal integration test; never uses a real profile.
#include <wx/app.h>
#include <cstdio>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <sys/resource.h>
#include <wx/file.h>
#include <wx/log.h>
#include "CamuleFileConfig.h"
#include "Preferences.h"
#include "FolderAccess.h"

wxString CPreferences::s_configDir;
CPath CPreferences::s_tempdir;
CPath CPreferences::s_incomingdir;

class PortalTestApp : public wxApp
{
public:
	bool OnInit() override { return true; }
};
wxIMPLEMENT_APP_NO_MAIN(PortalTestApp);

bool CheckAtomicSave(const wxString &profile)
{
	const wxString path = profile + "/atomic.conf";
	{
		wxFile original;
		if (!original.Create(path, true, 0600) || !original.Write("old=1\n")) {
			return false;
		}
	}
	CamuleFileConfig config("", "", path);
	config.Write("value", wxString('x', 8192));
	struct rlimit previous = {};
	if (getrlimit(RLIMIT_FSIZE, &previous) != 0) {
		return false;
	}
	struct rlimit limited = previous;
	limited.rlim_cur = 8;
	std::signal(SIGXFSZ, SIG_IGN);
	if (setrlimit(RLIMIT_FSIZE, &limited) != 0) {
		return false;
	}
	const bool rejected = !config.Flush();
	if (setrlimit(RLIMIT_FSIZE, &previous) != 0) {
		return false;
	}
	wxString originalContents;
	{
		wxFile original(path);
		if (!original.ReadAll(&originalContents)) {
			return false;
		}
	}
	const bool retry = config.Flush();
	return rejected && originalContents == "old=1\n" && retry && FolderAccess::BackupConfig(path) &&
	       CPath::BackupFile(CPath(path), ".credits.bak");
}

int main(int argc, char **argv)
{
	if (argc != 4 || !wxEntryStart(argc, argv)) {
		return 77;
	}
	wxTheApp->CallOnInit();
	delete wxLog::SetActiveTarget(new wxLogStderr());
	CPreferences::SetConfigDir(wxString::FromUTF8(argv[1]) + "/");
	wxString sandboxError;
	if (!FolderAccess::RestrictWrites(sandboxError)) {
		std::fprintf(stderr, "%s\n", sandboxError.utf8_str().data());
		wxEntryCleanup();
		return 77;
	}
	if (std::string(argv[3]) == "atomic") {
		const bool ok = CheckAtomicSave(wxString::FromUTF8(argv[1]));
		wxTheApp->OnExit();
		wxEntryCleanup();
		return ok ? 0 : 1;
	}
	if (std::string(argv[3]) == "registry") {
		const wxString base = wxString::FromUTF8(argv[2]);
		const wxString document = base + "/readonly/budget.xlsx";
		int probe = open(document.fn_str(), O_RDONLY);
		bool ok = probe >= 0;
		if (probe >= 0) {
			close(probe);
		}
		probe = open(document.fn_str(), O_WRONLY);
		ok = (probe < 0 && errno == EACCES) && ok;
		if (probe >= 0) {
			close(probe);
		}
		probe = open((base + "/downloads/download.part").fn_str(), O_CREAT | O_WRONLY, 0600);
		ok = probe >= 0 && ok;
		if (probe >= 0) {
			ok = write(probe, "partial download", 16) == 16 && ok;
			close(probe);
		}
		wxTheApp->OnExit();
		wxEntryCleanup();
		return ok ? 0 : 1;
	}
	const wxString result = FolderAccess::Choose(
		nullptr, "Approve test folder", wxString::FromUTF8(argv[2]), std::string(argv[3]) == "write");
	bool permissionMatches = true;
	if (!result.empty()) {
		const int probe = open((result + "/write-probe").fn_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
		permissionMatches =
			std::string(argv[3]) == "write" ? probe >= 0 : probe < 0 && errno == EACCES;
		if (probe >= 0) {
			close(probe);
		}
	}
	std::printf("SELECTED=%s\n", result.utf8_str().data());
	if (!result.empty()) {
		std::printf("HOST=%s\n", FolderAccess::HostPath(result).utf8_str().data());
	}
	wxTheApp->OnExit();
	wxEntryCleanup();
	return permissionMatches ? 0 : 1;
}
