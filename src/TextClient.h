//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2003-2011 Angel Vidal ( kry@amule.org )
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

#ifndef TEXTCLIENT_H
#define TEXTCLIENT_H

#include "ExternalConnector.h"

#include <map>

class CEC_SearchFile_Tag;

class SearchFile
{
public:
	wxString sFileName;
	// lFileSize must be 64-bit -- `unsigned long` is 32 bits on
	// LLP64 (Windows 64-bit), so amulecmd would mis-display the
	// size of any shared file > 4 GiB. The search EC tag carries
	// the size as uint64, so widen this carrier to match.
	uint64 lFileSize;
	CMD4Hash nHash;
	wxString sHash;
	long lSourceCount;
	bool bPresent;

	SearchFile(const CEC_SearchFile_Tag *);

	static class SearchInfo *GetContainerInstance();
	CMD4Hash ID() { return nHash; }
};

typedef std::map<unsigned long int, SearchFile *> CResultMap;

wxString ECv2_Response2String(CECPacket *response);

class CamulecmdApp : public CaMuleExternalConnector
{
public:
	const wxString GetGreetingTitle() { return _("aMule text client"); }
	int ProcessCommand(int ID);
	void Process_Answer_v2(const CECPacket *reply);
	void OnInitCommandSet();

private:
	// other command line switches
	void OnInitCmdLine(wxCmdLineParser &amuleweb_parser);
	bool OnCmdLineParsed(wxCmdLineParser &parser);
	void TextShell(const wxString &prompt);
	void ShowResults(CResultMap results_map);
	bool m_HasCmdOnCmdLine;
	wxString m_CmdString;

	/**
	 * Print list fields separated by spaces, as before, instead of tabs.
	 *
	 * The rows carry client names and file names, both of which routinely
	 * contain spaces, so a space-separated row cannot be split back into its
	 * fields by anything reading it (discussion #161). Tabs are the default;
	 * this restores the previous output byte for byte for scripts written
	 * against it.
	 */
	bool m_spaceSeparated;

	/**
	 * The separator to print where @a legacy used to be printed.
	 *
	 * ONLY separators go through here. Column padding stays as it is, values
	 * that merely contain punctuation (`12/34` source counts, `5(7)` request
	 * counts) stay one field, and translated values (file status, priority)
	 * are still translated -- a parser wanting stable text still wants
	 * LC_ALL=C, or the REST API.
	 */
	wxString Sep(const wxString &legacy) const { return m_spaceSeparated ? legacy : wxString(wxT("\t")); }
	virtual int OnRun();

	int m_last_cmd_id;
	CResultMap m_Results_map;
};

#endif // TEXTCLIENT_H
// File_checked_for_headers
