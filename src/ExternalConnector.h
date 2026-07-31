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

/*
 * This file must be included with wxUSE_GUI defined to zero or one.
 * Usually on console applications, this will be taken care of in
 * configure time. This is because wx classes will be compiled
 * differently in each case.
 *
 */

#ifndef __EXTERNALCONNECTOR_H__
#define __EXTERNALCONNECTOR_H__

#include <wx/app.h>     // For wxApp
#include <wx/cmdline.h> // For wxCmdLineEntryDesc
#include <ec/cpp/RemoteConnect.h>

#include <wx/intl.h>

#define CMD_DEPRECATED 0x1000
#define CMD_OK 0
#define CMD_ID_QUIT -1
#define CMD_ID_HELP -2
#define CMD_ERR_SYNTAX -3
#define CMD_ERR_PROCESS_CMD -4
#define CMD_ERR_NO_PARAM -5
#define CMD_ERR_MUST_HAVE_PARAM -6
#define CMD_ERR_INVALID_ARG -7
#define CMD_ERR_INCOMPLETE -8

enum Params
{
	CMD_PARAM_NEVER,
	CMD_PARAM_OPTIONAL,
	CMD_PARAM_ALWAYS
};

class CCommandTree;

typedef std::list<const CCommandTree *> CmdList_t;
typedef std::list<const CCommandTree *>::iterator CmdPos_t;
typedef std::list<const CCommandTree *>::const_iterator CmdPosConst_t;

class CaMuleExternalConnector;

class CCommandTree
{
public:
	CCommandTree(CaMuleExternalConnector &app)
	: m_command("")
	, m_cmd_id(CMD_ERR_SYNTAX)
	, m_short("")
	, m_verbose("")
	, m_params(CMD_PARAM_OPTIONAL)
	, m_parent(NULL)
	{
		m_app = &app;
	}

	~CCommandTree();

	CCommandTree *AddCommand(const wxString &command,
		int cmd_id,
		const wxString &shortDesc,
		const wxString &longDesc,
		enum Params params = CMD_PARAM_OPTIONAL)
	{
		return AddCommand(new CCommandTree(command, cmd_id, shortDesc, longDesc, params));
	}

	int FindCommandId(const wxString &command, wxString &args, wxString &cmdstr) const;
	wxString GetFullCommand() const;
	void PrintHelpFor(const wxString &command) const;

#ifdef HAVE_LIBREADLINE
	const CmdList_t *GetSubCommandsFor(const wxString &command, bool mayRestart = true) const;
	const wxString &GetCommand() const { return m_command; }
#endif

private:
	CCommandTree(const wxString &command,
		int cmd_id,
		const wxString &shortDesc,
		const wxString &longDesc,
		enum Params params)
	: m_command(command)
	, m_cmd_id(cmd_id)
	, m_short(shortDesc)
	, m_verbose(longDesc)
	, m_params(params)
	, m_parent(NULL)
	{
	}

	CCommandTree *AddCommand(CCommandTree *cmdTree);

	wxString m_command;
	int m_cmd_id;
	wxString m_short;
	wxString m_verbose;
	enum Params m_params;
	const CCommandTree *m_parent;
	CmdList_t m_subcommands;

	static CaMuleExternalConnector *m_app;
};

class CECFileConfig;

class CaMuleExternalConnector : public wxApp
{
public:
	//
	// Constructor & Destructor
	//
	CaMuleExternalConnector();
	~CaMuleExternalConnector();

	//
	// Virtual functions
	//
	virtual void Pre_Shell() {}
	virtual void Post_Shell() {}
	virtual int ProcessCommand(int) { return -1; }
	virtual void TextShell(const wxString &prompt);
	virtual void LoadConfigFile();
	virtual void SaveConfigFile();

	/**
	 * Whether this connector keeps its own remote.conf.
	 *
	 * True for amulecmd and amuleweb, which have no config file of their own.
	 * amuleapi returns false: it owns amuleapi.conf, and reading remote.conf
	 * as well gave two files describing the same EC connection, with the
	 * winner decided by load order. When false the base class registers
	 * neither --config-file, --write-config nor --create-config-from, and
	 * loads nothing -- the subclass is the only source of configuration.
	 */
	virtual bool UsesConnectorConfigFile() const { return true; }

	/**
	 * Whether this connector offers -P / --password on the command line.
	 *
	 * amuleapi returns false. A password passed there lands in argv, which
	 * any local user can read out of ps, and amuleapi has two ways to get
	 * the credential that do not: the ephemeral token the core writes when
	 * it spawns amuleapi, and [EC]/Password in its own amuleapi.conf.
	 * Keeping the option registered would keep offering the one route that
	 * leaks.
	 *
	 * amulecmd and amuleweb keep it. Both are commonly run by hand, and
	 * neither owns a config file that could hold the value instead.
	 */
	virtual bool UsesEcPasswordOption() const { return true; }

	/**
	 * Filename whose presence in <cwd>/config marks a portable install.
	 *
	 * Defaults to this connector's own config file (remote.conf). A
	 * subclass that keeps its settings elsewhere overrides it: probing for
	 * a file the tool never reads or writes can only detect portability by
	 * accident, when some other tool happens to have left one behind.
	 */
	virtual wxString PortableProbeFile() const { return "remote.conf"; }
	virtual void LoadAmuleConfig(CECFileConfig &cfg);
	virtual void OnInitCommandSet();
	virtual bool OnInit();
	virtual const wxString GetGreetingTitle() = 0;

	//
	// Other functions
	//
	void Show(const wxString &s);
	void DebugShow(const wxString &s)
	{
		if (m_Verbose)
			Show(s);
	}
	const wxString &GetCmdArgs() const { return m_cmdargs; }
	const wxString &GetLastCmdStr() const { return m_lastcmdstr; }
	int GetIDFromString(const wxString &buffer)
	{
		return m_commands.FindCommandId(buffer, m_cmdargs, m_lastcmdstr);
	}
	void Process_Answer(const wxString &answer);
	bool Parse_Command(const wxString &buffer);
	void GetCommand(const wxString &prompt, char *buffer, size_t buffer_size);
	const CECPacket *SendRecvMsg_v2(const CECPacket *request)
	{
		return m_ECClient->SendRecvPacket(request);
	}
	void SendPacket(const CECPacket *request) { m_ECClient->SendPacket(request); }
	bool IsServerPartialUpdateActive() const { return m_ECClient->ServerSupportsPartialUpdate(); }
	// Version string of the connected core, from the EC AUTH_OK handshake.
	// Empty when not connected (m_ECClient null) or when the daemon is old
	// enough to omit the EC_TAG_SERVER_VERSION tag.
	wxString GetServerVersion() const { return m_ECClient ? m_ECClient->GetServerVersion() : wxString(); }
	void ConnectAndRun(const wxString &ProgName, const wxString &ProgVersion);
	void ShowGreet();

	//
	// Command line processing
	//
	void OnInitCmdLine(wxCmdLineParser &amuleweb_parser, const char *appname);
	bool OnCmdLineParsed(wxCmdLineParser &parser);

#if wxUSE_ON_FATAL_EXCEPTION
	// Exception and assert handling
	void OnFatalException();
#endif
#ifdef __WXDEBUG__
	void OnAssertFailure(
		const wxChar *file, int line, const wxChar *func, const wxChar *cond, const wxChar *msg);
#endif

protected:
	// Set current locale, if language is not empty.
	// returns canonical name of set (current) locale
	virtual wxString SetLocale(const wxString &language);

	CECFileConfig *m_configFile;
	wxString m_configDir;
	long m_port;
	wxString m_host;
	CMD4Hash m_password;
	bool m_ZLIB;
	// Force ZLIB regardless of dialed-IP locality (#728 follow-up).
	// Set by `/EC/ForceZLIB=1` in the config or `--force-zlib` on the
	// CLI. Use case: a WireGuard tunnel endpoint that resolves to an
	// RFC1918 IP but whose transit is slow Internet — the locality
	// check would otherwise strip ZLIB and the user loses the perf
	// they actually want.
	bool m_forceZLIB;

	// Offer EC transport encryption. On by default: the daemon only encrypts
	// what a client asks for, and only the client knows what address it
	// dialed, so the decision has to live here rather than in the daemon --
	// the same reasoning the ZLIB locality hint already follows. This is the
	// opt-OUT, reachable as --disable-ec-encryption or /EC/Encryption=0.
	bool m_ECEncryption;
	// Advertise the multi-search EC capability (EC_TAG_CAN_MULTI_SEARCH) so
	// the daemon addresses searches by ID and several can run at once. Off by
	// default; only a connector that reads EC_TAG_SEARCH_ID back and handles
	// per-ID results (amulecmd) sets it true. amuleweb stays single-search.
	bool m_canMultiSearch;
	bool m_KeepQuiet;
	bool m_Verbose;
	// --log-file / --no-log-file. Parsed for every connector; only amuleapi
	// acts on them (installs a stdout/stderr tee into the file).
	wxString m_logFile;
	bool m_noLogFile;
	bool m_interactive;
	CCommandTree m_commands;
	const char *m_appname;

private:
	wxString m_configFileName;
	wxString m_cmdargs;
	wxString m_lastcmdstr;
	CRemoteConnect *m_ECClient;
	char *m_InputLine;
	bool m_NeedsConfigSave;
	wxString m_language;
	wxLocale *m_locale;
	char *m_strFullVersion;
	char *m_strOSDescription;
};

#endif // __EXTERNALCONNECTOR_H__
// File_checked_for_headers
