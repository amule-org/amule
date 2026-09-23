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

#include <wx/tokenzr.h>
#include <wx/imaglist.h>
#include <wx/datetime.h>

#include <wx/artprov.h>   // Needed for wxArtProvider::GetBitmap
#include "ChatSelector.h" // Interface declarations
#include "Preferences.h"  // Needed for CPreferences
#include "amule.h"        // Needed for theApp
#include "ClientRef.h"    // Needed for CClientRef
#include "OtherFunctions.h"
#include "UserEvents.h"
#include "Constants.h" // Needed for MS_NONE

// #warning Needed while not ported
#include "ClientList.h"
#include <common/Format.h> // Needed for CFormat

// Default colors,
#define COLOR_BLACK wxTextAttr(wxColor(0, 0, 0))
#define COLOR_BLUE wxTextAttr(wxColor(0, 0, 255))
#define COLOR_GREEN wxTextAttr(wxColor(0, 102, 0))
#define COLOR_RED wxTextAttr(wxColor(255, 0, 0))

CChatSession::CChatSession(wxWindow *parent,
	wxWindowID id,
	const wxString &value,
	const wxPoint &pos,
	const wxSize &size,
	long style,
	const wxValidator &validator,
	const wxString &name)
: CMuleTextCtrl(
	  parent, id, value, pos, size, style | wxTE_READONLY | wxTE_RICH | wxTE_MULTILINE, validator, name)
{
	m_client_id = {};
	m_active = false;
	SetBackgroundColour(*wxWHITE);
}

CChatSession::~CChatSession()
{
// #warning EC NEEDED
#ifndef CLIENT_GUI
	theApp->clientlist->SetChatState(m_client_id, MS_NONE);
#endif
}

void CChatSession::AddText(const wxString &text, const wxTextAttr &style, bool newline)
{
	// Split multi-line messages into individual lines
	wxStringTokenizer tokens(text, "\n");

	while (tokens.HasMoreTokens()) {
		// Check if we should add a time-stamp
		if (GetNumberOfLines() > 1) {
			// Check if the last line ended with a newline
			wxString line = GetLineText(GetNumberOfLines() - 1);
			if (line.IsEmpty()) {
				SetDefaultStyle(COLOR_BLACK);

				AppendText(" [" + wxDateTime::Now().Format("%X") + "] ");
			}
		}

		SetDefaultStyle(style);

		AppendText(tokens.GetNextToken());

		// Only add newlines after the last line if it is desired
		if (tokens.HasMoreTokens() || newline) {
			AppendText("\n");
		}
	}
}

CChatSelector::CChatSelector(wxWindow *parent, wxWindowID id, const wxPoint &pos, wxSize siz, long style)
: CMuleNotebook(parent, id, pos, siz, style)
{
	wxImageList *imagelist = new wxImageList(16, 16);

	// Chat icon -- default state
	imagelist->Add(wxArtProvider::GetBitmap("amule:chat"));
	// Close icon -- on mouseover
	imagelist->Add(ThemedCloseIcon(wxSize(16, 16)));

	AssignImageList(imagelist);
}

CChatSession *CChatSelector::StartSession(
	const CChatTarget &client_id, const wxString &client_name, bool show)
{
	if (!ChatTargetValid(client_id)) {
		return nullptr;
	}
	// Check to see if we've already opened a session for this user
	if (GetPageByClientID(client_id)) {
		if (show) {
			SetSelection(GetTabByClientID(client_id));
		}

		return NULL;
	}

	CChatSession *chatsession = new CChatSession(this);

	// Keep a value snapshot: promotion is delivered explicitly by RekeySession.
	chatsession->m_client_id = CChatPeer(client_id.Hash(), client_id.Address(), client_id.Port());

	// The title identifies the peer, not its mutable route.
	const wxString text = wxString(" *** ") +
			      wxString(CFormat(_("Chat-Session Started: %s - %s %s")) % client_name %
				       FormatLocalDate(wxDateTime::Now()) % wxDateTime::Now().Format("%X"));

	chatsession->AddText(text, COLOR_RED);
	AddPage(chatsession, client_name, show, 0);

	CUserEvents::ProcessEvent(CUserEvents::NewChatSession, &client_name);

	return chatsession;
}

void CChatSelector::RekeySession(const CChatTarget &old_id, const CChatTarget &new_id)
{
	if (old_id == new_id || !ChatTargetValid(new_id)) {
		return;
	}
	CChatSession *oldPage = GetPageByClientID(old_id);
	if (!oldPage) {
		return;
	}
	CChatSession *newPage = GetPageByClientID(new_id);
	if (newPage) {
		// Preserve both visible histories without issuing a core close notification.
		newPage->AppendText(oldPage->GetValue());
		newPage->m_active = newPage->m_active || oldPage->m_active;
		const int oldTab = GetTabByClientID(old_id);
		const bool selected = GetSelection() == oldTab;
		RemovePage(oldTab);
		oldPage->Destroy();
		if (selected) {
			SetSelection(GetTabByClientID(new_id));
		}
	} else {
		oldPage->m_client_id = new_id;
	}
}

CChatSession *CChatSelector::GetPageByClientID(const CChatTarget &client_id)
{
	for (unsigned int i = 0; i < (unsigned int)GetPageCount(); i++) {
		CChatSession *page = static_cast<CChatSession *>(GetPage(i));

		if (page->m_client_id == client_id) {
			return page;
		}
	}

	return NULL;
}

int CChatSelector::GetTabByClientID(const CChatTarget &client_id)
{
	for (unsigned int i = 0; i < (unsigned int)GetPageCount(); i++) {
		CChatSession *page = static_cast<CChatSession *>(GetPage(i));

		if (page->m_client_id == client_id) {
			return i;
		}
	}

	return -1;
}

bool CChatSelector::ProcessMessage(const CChatTarget &sender_id, const wxString &message)
{
	if (!ChatTargetValid(sender_id)) {
		return false;
	}
	CChatSession *session = GetPageByClientID(sender_id);

	// Try to get the name (core sent it?)
	int separator = message.Find("|");
	wxString client_name;
	wxString client_message;
	if (separator != -1) {
		client_name = message.Left(separator);
		client_message = message.Mid(separator + 1);
	} else {
		// No need to define client_name. If needed, will be build on tab creation.
		client_message = message;
	}

	bool newtab = !session;

	if (!session) {
		// This must be a message from a client that is not already chatting
		if (client_name.IsEmpty()) {
			// The core did not send us the name, which must NOT happen. Build a
			// client name from the ID.
			client_name = ChatPeerFallbackName(sender_id);
		}

		session = StartSession(sender_id, client_name, true);
	}

	// Other client connected after disconnection or a new session
	if (!session->m_active) {
		session->m_active = true;

		session->AddText(_("*** Connected to Client ***"), COLOR_RED);
	}

	// Page text is client name
	session->AddText(GetPageText(GetTabByClientID(sender_id)), COLOR_BLUE, false);
	session->AddText(": " + client_message, COLOR_BLACK);

	return newtab;
}

void CChatSelector::AppendStoredMessage(
	const CChatTarget &gui_id, const wxString &name, const wxString &text, bool outgoing)
{
	CChatSession *session = GetPageByClientID(gui_id);
	if (!session) {
		// show=false: a session can appear on its own -- opened by another client, or by a
		// peer messaging us -- and must not pull the selection away from whatever the local
		// user is doing.
		session = StartSession(gui_id, name, false);
		if (!session) {
			return;
		}
	}
	session->m_active = true;
	session->AddText(
		outgoing ? thePrefs::GetUserNick() : name, outgoing ? COLOR_GREEN : COLOR_BLUE, false);
	session->AddText(": " + text, COLOR_BLACK);
}

bool CChatSelector::SendMessage(
	const wxString &message, const wxString &client_name, const CChatTarget &to_id)
{
	// Dont let the user send empty messages
	// This is also a user-fix for people who mash the enter-key ...
	if (message.IsEmpty()) {
		return false;
	}

	if (ChatTargetValid(to_id)) {
		// Checks if there's a page with this client, and selects it or creates it
		StartSession(to_id, client_name, true);
	}

	int usedtab = GetSelection();
	// Workaround for a problem with wxNotebook, where an invalid selection is returned
	if (usedtab >= (int)GetPageCount()) {
		usedtab = GetPageCount() - 1;
	}
	if (usedtab == -1) {
		return false;
	}

	CChatSession *ci = static_cast<CChatSession *>(GetPage(usedtab));

	ci->m_active = true;

#ifdef CLIENT_GUI
	// amulegui sends through the daemon and deliberately does NOT echo the line locally: the
	// core records every outbound message in the chat session store, so the next
	// EC_OP_GET_CHAT_SESSIONS poll returns it and renders it here. Echoing as well would print
	// it twice, and printing it from the poll is also what keeps the ordering the core sees.
	CECPacket req(EC_OP_CHAT_SEND);
	req.AddTag(CECTag(EC_TAG_CHAT, message));
	AddChatTargetTags(req, ci->m_client_id);
	theApp->m_connect->SendPacket(&req);
#else
	const auto result = theApp->clientlist->SendChatMessage(ci->m_client_id, message);
	if (result == CClientList::ChatSendResult::Unavailable) {
		ci->AddText(
			_("*** Chat unavailable: peer identity or route is not available ***"), COLOR_RED);
		return false;
	}
	if (result == CClientList::ChatSendResult::Sent) {
		ci->AddText(thePrefs::GetUserNick(), COLOR_GREEN, false);
		ci->AddText(": " + message, COLOR_BLACK);
	} else {
		ci->AddText(_("*** Connecting to Client ***"), COLOR_RED);
	}
#endif

	return true;
}

// #warning Creteil?  I know you are here Creteil... follow the white rabbit.
/* Madcat - knock knock ...
	   ,-.,-.
	    \ \\ \
	     \ \\_\
	     /     \
	  __|    a a|
	/`   `'. = y)=
       /        `"`}
     _|    \       }
    { \     ),   //
     '-',  /__\ ( (
   jgs (______)\_)_)
*/

void CChatSelector::ConnectionResult(bool success, const wxString &message, const CChatTarget &id)
{
	CChatSession *ci = GetPageByClientID(id);
	if (!ci) {
		return;
	}

	if (!success) {
		ci->AddText(_("*** Failed to Connect to client / Connection lost ***"), COLOR_RED);

		ci->m_active = false;
	} else {
		// Kry - Woops, fix for the everlasting void message sending.
		if (!message.IsEmpty()) {
			ci->AddText(_("*** Connected to Client ***"), COLOR_RED);
			ci->AddText(thePrefs::GetUserNick(), COLOR_GREEN, false);
			ci->AddText(": " + message, COLOR_BLACK);
		}
	}
}

void CChatSelector::EndSession(const CChatTarget &client_id)
{
	int usedtab;
	if (ChatTargetValid(client_id)) {
		usedtab = GetTabByClientID(client_id);
	} else {
		usedtab = GetSelection();
	}

	if (usedtab == -1) {
		return;
	}

	DeletePage(usedtab);
}

// Refresh the tab associated with a client
void CChatSelector::RefreshFriend(const CChatTarget &toupdate_id, const wxString &new_name)
{
	if (!ChatTargetValid(toupdate_id)) {
		return;
	}

	int tab = GetTabByClientID(toupdate_id);

	if (tab != -1) {
		// This client has a tab.
		SetPageText(tab, new_name);
	} else {
		// This client has no tab (friend disconnecting, etc)
		// Nothing to be done here.
	}
}

void CChatSelector::ShowCaptchaResult(const CChatTarget &id, bool ok)
{
	CChatSession *ci = GetPageByClientID(id);
	if (ci) {
		ci->AddText(ok ? _("*** You have passed the captcha check and the user has received your "
				   "message. ***")
			       : _("*** Your response to the captcha was wrong and your message has been "
				   "ignored. You can request a new captcha by sending a new message. ***"),
			COLOR_RED);
	}
}

#ifdef CLIENT_GUI
bool CChatSelector::GetCurrentClient(CClientRef &) const
{
	return false;
}
#else
bool CChatSelector::GetCurrentClient(CClientRef &clientref) const
{
	// Get the chat session associated with the active tab
	CChatSession *ci = static_cast<CChatSession *>(GetPage(GetSelection()));

	// Get the client that the session is open to
	if (ci) {
		CUpDownClient *client = theApp->clientlist->FindChatClient(ci->m_client_id);
		if (client) {
			clientref.Link(client CLIENT_DEBUGSTRING("CChatSelector::GetCurrentClient"));
			return true;
		}
	}
	return false;
}
#endif

wxString ChatPeerFallbackName(const CChatPeer &peer)
{
	if (!peer.Hash().IsEmpty()) {
		return peer.Hash().Encode();
	}
	return CFormat(wxT("IP: %s Port: %u")) % Uint32toStringIP(peer.Address().ToIPv4NetworkOrderOrZero()) %
	       peer.Port();
}

#ifdef CLIENT_GUI
void AddChatTargetTags(CECPacket &req, const CChatTarget &target)
{
	const uint32 ip = target.Address().ToIPv4NetworkOrderOrZero();
	if (ip && target.Port()) {
		req.AddTag(CECTag(EC_TAG_CHAT_CLIENT_ID, GUI_ID(ip, target.Port())));
	}
	if (!target.Hash().IsEmpty() && theApp->m_connect &&
		theApp->m_connect->ServerSupportsChatPeerHash()) {
		req.AddTag(CECTag(EC_TAG_CHAT_PEER_HASH, target.Hash()));
	}
}
#endif

// File_checked_for_headers
