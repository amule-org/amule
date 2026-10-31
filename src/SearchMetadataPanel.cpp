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

#include "SearchMetadataPanel.h"

#include <ec/cpp/ECSpecialTags.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <algorithm>

CSearchMetadataPanel::CSearchMetadataPanel(wxWindow *parent)
: wxPanel(parent)
{
	const std::array<wxString, FieldCount> labels = { _("Artist"), _("Album"), _("Title"), _("Codec") };
	auto *grid = new wxFlexGridSizer(4, FromDIP(6), FromDIP(12));
	grid->AddGrowableCol(1);
	grid->AddGrowableCol(3);
	for (size_t i = 0; i < m_fields.size(); ++i) {
		grid->Add(new wxStaticText(this, wxID_ANY, labels[i]), wxSizerFlags().CenterVertical());
		m_fields[i] = new wxTextCtrl(this,
			wxID_ANY,
			wxEmptyString,
			wxDefaultPosition,
			FromDIP(wxSize(130, -1)),
			wxTE_PROCESS_ENTER);
		m_fields[i]->SetName(labels[i]);
		grid->Add(m_fields[i], wxSizerFlags(1).Expand());
	}
	const auto addNumber = [this, grid](const wxString &label, int maximum) {
		grid->Add(new wxStaticText(this, wxID_ANY, label), wxSizerFlags().CenterVertical());
		auto *field = new wxSpinCtrl(this,
			wxID_ANY,
			wxEmptyString,
			wxDefaultPosition,
			FromDIP(wxSize(130, -1)),
			wxSP_ARROW_KEYS | wxTE_PROCESS_ENTER,
			0,
			maximum,
			0);
		field->SetName(label);
		grid->Add(field, wxSizerFlags(1).Expand());
		return field;
	};
	m_length = addNumber(_("Minimum duration (seconds)"), SEARCH_MAX_MIN_DURATION_SECONDS);
	m_bitrate = addNumber(_("Minimum bitrate (kbps)"), SEARCH_MAX_MIN_BITRATE_KBPS);
	m_help = new wxStaticText(this, wxID_ANY, wxEmptyString);
	auto *layout = new wxBoxSizer(wxVERTICAL);
	layout->Add(grid, wxSizerFlags().Expand());
	layout->Add(m_help, wxSizerFlags().Expand().Border(wxTOP, FromDIP(6)));
	SetSizer(layout);
	SetSupported(true);
}

void CSearchMetadataPanel::Read(CSearchList::CSearchParams &params) const
{
	params.artist = m_fields[Artist]->GetValue();
	params.album = m_fields[Album]->GetValue();
	params.title = m_fields[Title]->GetValue();
	params.codec = m_fields[Codec]->GetValue();
	params.minLength = m_length->GetValue();
	params.minBitrateKbps = m_bitrate->GetValue();
}

void CSearchMetadataPanel::Read(CSearchMetadata &metadata) const
{
	CSearchList::CSearchParams params;
	Read(params);
	metadata.artist = params.artist;
	metadata.album = params.album;
	metadata.title = params.title;
	metadata.codec = params.codec;
	metadata.minLength = params.minLength;
	metadata.minBitrateKbps = params.minBitrateKbps;
}

void CSearchMetadataPanel::Set(const CSearchMetadata &metadata)
{
	m_fields[Artist]->ChangeValue(metadata.artist);
	m_fields[Album]->ChangeValue(metadata.album);
	m_fields[Title]->ChangeValue(metadata.title);
	m_fields[Codec]->ChangeValue(metadata.codec);
	m_length->SetValue(static_cast<int>(metadata.minLength));
	m_bitrate->SetValue(static_cast<int>(metadata.minBitrateKbps));
}

bool CSearchMetadataPanel::HasValues() const
{
	return m_length->GetValue() != 0 || m_bitrate->GetValue() != 0 ||
	       std::any_of(m_fields.begin(), m_fields.end(), [](const wxTextCtrl *field) {
		       return !field->GetValue().empty();
	       });
}

void CSearchMetadataPanel::Reset()
{
	for (wxTextCtrl *field : m_fields)
		field->Clear();
	m_length->SetValue(0);
	m_bitrate->SetValue(0);
}

void CSearchMetadataPanel::SetSupported(bool supported)
{
	for (wxTextCtrl *field : m_fields)
		field->Enable(supported);
	m_length->Enable(supported);
	m_bitrate->Enable(supported);
	m_help->SetLabel(
		supported ? _("Kad applies these filters; eD2k servers may ignore them")
			  : _("The connected daemon does not support media filters. Update the daemon to use "
			      "these fields."));
	m_help->Wrap(FromDIP(650));
	Layout();
}
