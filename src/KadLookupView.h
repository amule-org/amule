//								-*- C++ -*-
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

#ifndef KAD_LOOKUP_VIEW_H
#define KAD_LOOKUP_VIEW_H

#include <wx/wx.h>
#include <wx/weakref.h>

// Parent-owned, modeless snapshot: connection controls stay accessible while reading.
class CKadLookupView final : public wxDialog
{
	wxTextCtrl *m_text;
	bool m_pending = false;

public:
	explicit CKadLookupView(wxWindow *parent)
	: wxDialog(parent,
		  wxID_ANY,
		  _("Kad lookup diagnostics"),
		  wxDefaultPosition,
		  parent->FromDIP(wxSize(850, 500)),
		  wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
	{
		auto *sizer = new wxBoxSizer(wxVERTICAL);
		m_text = new wxTextCtrl(this,
			wxID_ANY,
			wxEmptyString,
			wxDefaultPosition,
			wxDefaultSize,
			wxTE_MULTILINE | wxTE_READONLY | wxHSCROLL);
		sizer->Add(m_text, wxSizerFlags(1).Expand().Border(wxALL, FromDIP(8)));
		sizer->Add(CreateButtonSizer(wxOK), wxSizerFlags().Expand().Border(wxALL, FromDIP(8)));
		SetSizer(sizer);
		Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { Destroy(); }, wxID_OK);
		Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent &) { Destroy(); });
	}
	void SetSnapshot(const wxString &text) { m_text->ChangeValue(text); }
	void SetPending(bool pending) { m_pending = pending; }
	bool IsPending() const { return m_pending; }
	wxString GetSnapshot() const { return m_text->GetValue(); }
};

#ifdef CLIENT_GUI
#include "libs/ec/cpp/RemoteConnect.h"
#include <common/Format.h>
#include <initializer_list>

namespace KadLookupRemote
{
inline uint64_t Number(const CECTag &parent, ec_tagname_t name)
{
	const auto *tag = parent.GetTagByName(name);
	return tag && tag->IsInt() ? tag->GetInt() : 0;
}

inline bool ValidInts(const CECTag &parent, std::initializer_list<ec_tagname_t> names)
{
	for (auto name : names) {
		const auto *tag = parent.GetTagByName(name);
		if (!tag || !tag->IsInt()) {
			return false;
		}
	}
	return true;
}

inline bool ValidID(const CECTag &parent, ec_tagname_t name)
{
	const auto *tag = parent.GetTagByName(name);
	return tag && tag->IsCustom() && tag->GetTagDataLen() == 16;
}

inline wxString String(const CECTag &parent, ec_tagname_t name)
{
	const auto *tag = parent.GetTagByName(name);
	return tag && tag->IsString() ? tag->GetStringData() : wxString();
}

inline wxString Hex(const CECTag &parent, ec_tagname_t name)
{
	const auto *tag = parent.GetTagByName(name);
	if (!tag || !tag->IsCustom() || tag->GetTagDataLen() != 16) {
		return "?";
	}
	wxString value;
	const auto *bytes = static_cast<const uint8_t *>(tag->GetTagData());
	for (unsigned i = 0; i != 16; ++i) {
		value += wxString::Format("%02x", bytes[i]);
	}
	return value;
}

inline wxString Address(const CECTag &parent, ec_tagname_t ipName, ec_tagname_t portName)
{
	const uint32_t ip = Number(parent, ipName);
	return CFormat("%u.%u.%u.%u:%u") % uint8_t(ip >> 24) % uint8_t(ip >> 16) % uint8_t(ip >> 8) %
	       uint8_t(ip) % Number(parent, portName);
}

inline wxString SearchType(uint64_t type)
{
	switch (type) {
	case 0:
		return _("Node");
	case 1:
		return _("Complete node");
	case 2:
		return _("File");
	case 3:
		return _("Keyword");
	case 4:
		return _("Notes");
	case 5:
		return _("Publish file");
	case 6:
		return _("Publish keyword");
	case 7:
		return _("Publish notes");
	case 8:
		return _("Find buddy");
	case 9:
		return _("Find source");
	case 10:
		return _("Special node");
	case 11:
		return _("UDP firewall check");
	default:
		return CFormat(_("type %u")) % type;
	}
}

inline wxString FormatPacket(const CECPacket &packet)
{
	if (packet.GetOpCode() != EC_OP_GET_KAD_LOOKUPS) {
		return _("The core does not support Kad lookup diagnostics.");
	}
	wxString text = _("Overdue means a routing request unanswered for at least 3 seconds; late replies "
			  "may still arrive. Result records are received records, not unique files. Only "
			  "bounded recent history is retained.");
	text += "\n\n";
	bool any = false;
	for (const auto &lookup : packet) {
		if (lookup.GetTagName() != EC_TAG_KAD_LOOKUP) {
			return _("Invalid Kad lookup diagnostics reply.");
		}
		const auto *target = lookup.GetTagByName(EC_TAG_KAD_LOOKUP_TARGET);
		const auto *type = lookup.GetTagByName(EC_TAG_KAD_LOOKUP_TYPE);
		const auto *keywordTag = lookup.GetTagByName(EC_TAG_KAD_LOOKUP_KEYWORD);
		if (!target || !target->IsString() || !keywordTag || !keywordTag->IsString() || !type ||
			!type->IsInt() ||
			!ValidInts(lookup,
				{ EC_TAG_KAD_LOOKUP_ACTIVE,
					EC_TAG_KAD_LOOKUP_STARTED,
					EC_TAG_KAD_LOOKUP_OVERDUE,
					EC_TAG_KAD_LOOKUP_OMITTED_PEERS,
					EC_TAG_KAD_LOOKUP_EVICTED_REFERRALS })) {
			return _("Invalid Kad lookup diagnostics reply.");
		}
		any = true;
		unsigned peers = 0;
		for (const auto &child : lookup) {
			if (child.GetTagName() == EC_TAG_KAD_LOOKUP_PEER) {
				if (!ValidInts(child,
					    { EC_TAG_KAD_LOOKUP_PEER_IP,
						    EC_TAG_KAD_LOOKUP_PEER_PORT,
						    EC_TAG_KAD_LOOKUP_PEER_VERSION,
						    EC_TAG_KAD_LOOKUP_PEER_REQUESTS,
						    EC_TAG_KAD_LOOKUP_PEER_REPLIES,
						    EC_TAG_KAD_LOOKUP_PEER_RTT,
						    EC_TAG_KAD_LOOKUP_PEER_ITEM_REQUESTS,
						    EC_TAG_KAD_LOOKUP_PEER_ITEM_REPLIES,
						    EC_TAG_KAD_LOOKUP_PEER_RESULTS,
						    EC_TAG_KAD_LOOKUP_PEER_PENDING }) ||
					!ValidID(child, EC_TAG_KAD_LOOKUP_PEER_ID) ||
					!ValidID(child, EC_TAG_KAD_LOOKUP_PEER_DISTANCE)) {
					return _("Invalid Kad lookup diagnostics reply.");
				}
				++peers;
			} else if (child.GetTagName() == EC_TAG_KAD_LOOKUP_REFERRAL &&
				   (!ValidInts(child,
					    { EC_TAG_KAD_LOOKUP_REFERRAL_SOURCE_IP,
						    EC_TAG_KAD_LOOKUP_REFERRAL_SOURCE_PORT,
						    EC_TAG_KAD_LOOKUP_REFERRAL_PEER_IP,
						    EC_TAG_KAD_LOOKUP_REFERRAL_PEER_PORT,
						    EC_TAG_KAD_LOOKUP_REFERRAL_ELAPSED,
						    EC_TAG_KAD_LOOKUP_REFERRAL_CLOSER }) ||
					   !ValidID(child, EC_TAG_KAD_LOOKUP_REFERRAL_DISTANCE))) {
				return _("Invalid Kad lookup diagnostics reply.");
			}
		}
		text += CFormat(_("Lookup %s (%s, %s): %u peers, %u overdue routing requests, "
				  "%u omitted peers, %u evicted referrals")) %
			String(lookup, EC_TAG_KAD_LOOKUP_TARGET) %
			SearchType(Number(lookup, EC_TAG_KAD_LOOKUP_TYPE)) %
			(Number(lookup, EC_TAG_KAD_LOOKUP_ACTIVE) ? _("active lookup")
								  : _("finished lookup")) %
			peers % Number(lookup, EC_TAG_KAD_LOOKUP_OVERDUE) %
			Number(lookup, EC_TAG_KAD_LOOKUP_OMITTED_PEERS) %
			Number(lookup, EC_TAG_KAD_LOOKUP_EVICTED_REFERRALS);
		text += "\n";
		const wxString keyword = String(lookup, EC_TAG_KAD_LOOKUP_KEYWORD);
		if (!keyword.empty()) {
			text += "  ";
			text += CFormat(_("Keyword: %s")) % keyword;
			text += "\n";
		}
		for (const auto &child : lookup) {
			if (child.GetTagName() == EC_TAG_KAD_LOOKUP_PEER) {
				text += "  ";
				text += CFormat(_("%s [%s], distance %s, Kad version %u (%s): %u routing "
						  "requests, "
						  "%u replies, last RTT %u ms, %u item requests, %u result "
						  "packets, "
						  "%u result records")) %
					Address(child,
						EC_TAG_KAD_LOOKUP_PEER_IP,
						EC_TAG_KAD_LOOKUP_PEER_PORT) %
					Hex(child, EC_TAG_KAD_LOOKUP_PEER_ID) %
					Hex(child, EC_TAG_KAD_LOOKUP_PEER_DISTANCE) %
					Number(child, EC_TAG_KAD_LOOKUP_PEER_VERSION) %
					(Number(child, EC_TAG_KAD_LOOKUP_PEER_PENDING)
							? _("awaiting reply")
							: _("reply received")) %
					Number(child, EC_TAG_KAD_LOOKUP_PEER_REQUESTS) %
					Number(child, EC_TAG_KAD_LOOKUP_PEER_REPLIES) %
					Number(child, EC_TAG_KAD_LOOKUP_PEER_RTT) %
					Number(child, EC_TAG_KAD_LOOKUP_PEER_ITEM_REQUESTS) %
					Number(child, EC_TAG_KAD_LOOKUP_PEER_ITEM_REPLIES) %
					Number(child, EC_TAG_KAD_LOOKUP_PEER_RESULTS);
				text += "\n";
			} else if (child.GetTagName() == EC_TAG_KAD_LOOKUP_REFERRAL) {
				text += "  ";
				text += CFormat(_("+%u ms: %s referred %s (distance %s, %s)")) %
					Number(child, EC_TAG_KAD_LOOKUP_REFERRAL_ELAPSED) %
					Address(child,
						EC_TAG_KAD_LOOKUP_REFERRAL_SOURCE_IP,
						EC_TAG_KAD_LOOKUP_REFERRAL_SOURCE_PORT) %
					Address(child,
						EC_TAG_KAD_LOOKUP_REFERRAL_PEER_IP,
						EC_TAG_KAD_LOOKUP_REFERRAL_PEER_PORT) %
					Hex(child, EC_TAG_KAD_LOOKUP_REFERRAL_DISTANCE) %
					(Number(child, EC_TAG_KAD_LOOKUP_REFERRAL_CLOSER)
							? _("closer to target")
							: _("no closer to target"));
				text += "\n";
			}
		}
		text += "\n";
	}
	if (!any) {
		text += _("No Kad lookup history available.");
	}
	return text;
}
} // namespace KadLookupRemote

class CKadLookupReply final : public CECPacketHandlerBase
{
	wxWeakRef<CKadLookupView> m_view;
	wxWeakRef<wxButton> m_button;

public:
	CKadLookupReply(CKadLookupView *view, wxButton *button)
	: m_view(view)
	, m_button(button)
	{
	}
	~CKadLookupReply() override
	{
		if (m_view) {
			m_view->SetPending(false);
		}
		if (m_button) {
			m_button->Enable();
		}
	}
	void HandlePacket(const CECPacket *packet) override
	{
		if (m_view) {
			m_view->SetSnapshot(KadLookupRemote::FormatPacket(*packet));
		}
		delete this;
	}
	void AbortPendingRequest() override
	{
		if (m_view) {
			m_view->SetSnapshot(
				_("Connection lost. Reconnect and request lookup diagnostics again."));
		}
		delete this;
	}
};
#endif
#endif
