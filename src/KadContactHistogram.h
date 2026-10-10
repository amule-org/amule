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

#ifndef AMULE_KADCONTACTHISTOGRAM_H
#define AMULE_KADCONTACTHISTOGRAM_H
#include <wx/panel.h>
#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/intl.h>
#include <wx/textwrapper.h>
#include <wx/sizer.h>
#include <vector>
#include <algorithm>
#include <utility>
#include "kademlia/utils/ContactDistribution.h"
// Snapshot view; drawing never traverses live contacts or changes routing.
class CKadContactHistogram final : public wxPanel
{
public:
	explicit CKadContactHistogram(wxWindow *parent)
	: wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
	{
		SetMinSize(FromDIP(wxSize(200, 0)));
		SetBackgroundStyle(wxBG_STYLE_PAINT);
		SetName("kadContactDistribution");
		SetToolTip(_("Contacts grouped by the first twelve bits of their KadID. Blue: all contacts; "
			     "green: verified addresses. The marker shows your own KadID; clustering near it "
			     "is expected."));
		Bind(wxEVT_PAINT, &CKadContactHistogram::Paint, this);
		Bind(wxEVT_SIZE, [this](wxSizeEvent &event) {
			UpdateMinimumSize();
			Refresh(false);
			event.Skip();
		});
		Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent &event) {
			event.Skip();
			CallAfter(&CKadContactHistogram::UpdateMinimumSize);
		});
		UpdateMinimumSize();
	}
	bool SetFont(const wxFont &font) override
	{
		if (!wxPanel::SetFont(font)) {
			return false;
		}
		UpdateMinimumSize();
		Refresh(false);
		return true;
	}
	void SetDistribution(
		const Kademlia::ContactDistribution &data, Kademlia::ContactDistributionState state)
	{
		if (m_state == state && m_data.contacts == data.contacts &&
			m_data.verified == data.verified && m_data.subnets == data.subnets &&
			m_data.localID == data.localID && m_data.hasLocalID == data.hasLocalID) {
			return;
		}
		m_data = data;
		m_state = state;
		UpdateMinimumSize();
		Refresh(false);
	}

private:
	friend struct KadHistogramTestAccess;
	class SummaryWrapper : public wxTextWrapper
	{
	public:
		std::vector<wxString> lines;
		void Wrap(wxWindow *window, const wxString &text, int width)
		{
			lines.clear();
			wxTextWrapper::Wrap(window, text, width);
			// wxTextWrapper leaves words wider than the panel unbroken. Split
			// those lines at Unicode characters so long translations/counts fit.
			wxClientDC dc(window);
			dc.SetFont(window->GetFont());
			std::vector<wxString> fitted;
			for (const auto &line : lines) {
				if (dc.GetTextExtent(line).x <= width) {
					fitted.push_back(line);
					continue;
				}
				wxString part;
				for (const wxUniChar character : line) {
					if (!part.empty() && dc.GetTextExtent(part + character).x > width) {
						fitted.push_back(part);
						part.clear();
					}
					part += character;
				}
				fitted.push_back(part);
			}
			lines = std::move(fitted);
		}

	protected:
		void OnOutputLine(const wxString &line) override { lines.push_back(line); }
	};
	wxString SummaryText() const
	{
		if (m_state == Kademlia::ContactDistributionState::Loading) {
			return _("Loading contact distribution...");
		}
		if (m_state == Kademlia::ContactDistributionState::Unsupported) {
			return _("This core does not send the contact distribution. Update the core to see "
				 "the chart.");
		}
		if (m_state == Kademlia::ContactDistributionState::Invalid) {
			return _("The core sent contact distribution data that could not be read.");
		}
		return wxString::Format(_("Routing-table contacts: %u; verified: %u; distinct /24s: %u"),
			m_data.Total(),
			m_data.Verified(),
			m_data.subnets);
	}
	void UpdateMinimumSize()
	{
		bool layoutChanged = false;
		if (auto *sizer = GetContainingSizer()) {
			if (auto *item = sizer->GetItem(this)) {
				const int proportion =
					m_state == Kademlia::ContactDistributionState::Available ? 2 : 0;
				if (item->GetProportion() != proportion) {
					item->SetProportion(proportion);
					layoutChanged = true;
				}
			}
		}
		wxClientDC dc(this);
		dc.SetFont(GetFont());
		int minimumWidth = FromDIP(200);
		if (m_state == Kademlia::ContactDistributionState::Available) {
			const int left = std::max(FromDIP(36),
				dc.GetTextExtent(wxString::Format("%u", m_data.Total())).x + FromDIP(6));
			const int plotWidth = std::max(FromDIP(64),
				dc.GetTextExtent("000").x + dc.GetTextExtent("fff").x + FromDIP(8));
			minimumWidth = std::max(minimumWidth, left + plotWidth + FromDIP(8));
		}
		SummaryWrapper wrapped;
		wrapped.Wrap(this,
			SummaryText(),
			std::max(1, std::max(minimumWidth, GetClientSize().x) - FromDIP(16)));
		int height = FromDIP(16) + static_cast<int>(wrapped.lines.size()) * dc.GetCharHeight();
		if (m_state == Kademlia::ContactDistributionState::Available) {
			height += FromDIP(40 + 7 + 6) + dc.GetCharHeight();
		}
		const wxSize minimum(minimumWidth, height);
		if (minimum != GetMinSize()) {
			SetMinSize(minimum);
			layoutChanged = true;
		}
		// Re-layout once the current resize/DPI event finishes. A changed size
		// or proportion must not recursively resize the parent in its sizer pass.
		if (layoutChanged && GetParent()) {
			GetParent()->CallAfter([parent = GetParent()] { parent->Layout(); });
		}
	}
	void Paint(wxPaintEvent &)
	{
		wxAutoBufferedPaintDC dc(this);
		Draw(dc, GetClientSize());
	}
	void Draw(wxDC &dc, const wxSize &size)
	{
		dc.SetBackground(wxBrush(GetBackgroundColour()));
		dc.Clear();
		dc.SetTextForeground(GetForegroundColour());
		dc.SetFont(GetFont());
		if (m_state != Kademlia::ContactDistributionState::Available) {
			SummaryWrapper wrapped;
			wrapped.Wrap(this, SummaryText(), std::max(1, size.x - FromDIP(16)));
			int y = FromDIP(8);
			for (const auto &line : wrapped.lines) {
				dc.DrawText(line, FromDIP(8), y);
				y += dc.GetCharHeight();
			}
			return;
		}
		// Summary wraps at narrow widths and with longer translations.
		SummaryWrapper wrapped;
		wrapped.Wrap(this, SummaryText(), std::max(1, size.x - FromDIP(16)));
		int y = FromDIP(5);
		for (const auto &line : wrapped.lines) {
			dc.DrawText(line, FromDIP(8), y);
			y += dc.GetCharHeight();
		}
		const int top = y + FromDIP(7);
		// Reserve label space using the total (an upper bound for any column).
		const int left = std::max(
			FromDIP(36), dc.GetTextExtent(wxString::Format("%u", m_data.Total())).x + FromDIP(6));
		const int bottom = size.y - dc.GetCharHeight() - FromDIP(6);
		const int width = size.x - left - FromDIP(8), height = bottom - top;
		if (width <= 0 || height <= 0) {
			return;
		}
		const size_t columns = std::min(static_cast<size_t>(width), m_data.BinCount);
		std::vector<uint32_t> contacts(columns), verified(columns);
		for (size_t i = 0; i < m_data.BinCount; ++i) {
			const size_t column = i * columns / m_data.BinCount;
			contacts[column] += m_data.contacts[i];
			verified[column] += m_data.verified[i];
		}
		const uint32_t peak = std::max(1u, *std::max_element(contacts.begin(), contacts.end()));
		const wxString peakLabel = wxString::Format("%u", peak);
		dc.SetPen(wxPen(GetForegroundColour()));
		dc.DrawLine(left, top, left, bottom);
		dc.DrawLine(left, bottom, left + width, bottom);
		dc.DrawText(peakLabel, FromDIP(2), top);
		for (size_t i = 0; i < columns; ++i) {
			const int x = left + static_cast<int>(i * width / columns);
			const int next = left + static_cast<int>((i + 1) * width / columns);
			const int barWidth = std::max(1, next - x);
			auto bar = [&](uint32_t count, const wxColour &colour) {
				const int h = static_cast<int>(static_cast<uint64_t>(count) * height / peak);
				if (h) {
					dc.SetPen(*wxTRANSPARENT_PEN);
					dc.SetBrush(wxBrush(colour));
					dc.DrawRectangle(x, bottom - h, barWidth, h);
				}
			};
			bar(contacts[i], wxColour(70, 130, 210));
			bar(verified[i], wxColour(35, 160, 90));
		}
		if (m_data.hasLocalID) {
			const int x = left + static_cast<int>(static_cast<uint64_t>(m_data.localID) *
							      (width - 1) / UINT32_MAX);
			const int radius = FromDIP(5);
			const int markerHeight = FromDIP(7);
			dc.SetPen(wxPen(GetForegroundColour(), FromDIP(1), wxPENSTYLE_SHORT_DASH));
			dc.DrawLine(x, top + markerHeight, x, bottom);
			dc.SetPen(*wxTRANSPARENT_PEN);
			dc.SetBrush(wxBrush(GetForegroundColour()));
			wxPoint marker[] = { wxPoint(x - radius, top),
				wxPoint(x + radius, top),
				wxPoint(x, top + markerHeight) };
			dc.DrawPolygon(3, marker);
		}
		dc.DrawText("000", left, bottom + FromDIP(2));
		dc.DrawText("fff", left + width - dc.GetTextExtent("fff").x, bottom + FromDIP(2));
	}
	Kademlia::ContactDistribution m_data;
	Kademlia::ContactDistributionState m_state = Kademlia::ContactDistributionState::Loading;
};
#endif
