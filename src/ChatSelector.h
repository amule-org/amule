//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2002-2011 Merkur ( devs@emule-project.net / http://www.emule-project.net )
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

#ifndef CHATSELECTOR_H
#define CHATSELECTOR_H

#include "MuleTextCtrl.h"
#include "MuleNotebook.h"
#include "ChatSessionStore.h"

class CClientRef;
class CFriend;

/**
 * Displays chat sessions.
 */
class CChatSession : public CMuleTextCtrl
{
public:
	CChatSession(wxWindow *parent,
		wxWindowID id = -1,
		const wxString &value = "",
		const wxPoint &pos = wxDefaultPosition,
		const wxSize &size = wxDefaultSize,
		long style = 0,
		const wxValidator &validator = wxDefaultValidator,
		const wxString &name = wxTextCtrlNameStr);
	~CChatSession();

	CChatTarget m_client_id;
	bool m_active;

	/**
	 * Appends the specified text.
	 *
	 * @param text The text to add.
	 * @param style The style of the new text.
	 * @param newline If a newline should be added to the end of the line.
	 *
	 * With @a newline false the added text never ends in a newline, even if the passed string
	 * does. Multiline strings are broken into individual lines, each timestamped with the same
	 * date.
	 */
	void AddText(const wxString &text, const wxTextAttr &style, bool newline = true);
};

class CChatSelector : public CMuleNotebook
{
public:
	CChatSelector(wxWindow *parent, wxWindowID id, const wxPoint &pos, wxSize siz, long style);
	virtual ~CChatSelector() {};
	CChatSession *StartSession(
		const CChatTarget &client_id, const wxString &client_name, bool show = true);
	void EndSession(const CChatTarget &client_id = {});
	void RekeySession(const CChatTarget &old_id, const CChatTarget &new_id);
	CChatSession *GetPageByClientID(const CChatTarget &client_id);
	int GetTabByClientID(const CChatTarget &client_id);
	bool ProcessMessage(const CChatTarget &sender_id, const wxString &message);

	/**
	 * Render one message the core's session store already holds.
	 *
	 * Distinct from ProcessMessage, which parses the "name|text" wire form and always announces
	 * the message. This takes an already decoded message with its direction, and does NOT touch
	 * the new-message blink -- the caller decides that, because replaying history on connect
	 * must not light the Messages button up for messages already read elsewhere.
	 */
	void AppendStoredMessage(
		const CChatTarget &gui_id, const wxString &name, const wxString &text, bool outgoing);
	bool SendMessage(
		const wxString &message, const wxString &client_name = "", const CChatTarget &to_id = {});
	void ConnectionResult(bool success, const wxString &message, const CChatTarget &id);
	void RefreshFriend(const CChatTarget &toupdate_id, const wxString &new_name);
	void ShowCaptchaResult(const CChatTarget &id, bool ok);
	bool GetCurrentClient(CClientRef &) const;
};

#endif
// File_checked_for_headers
