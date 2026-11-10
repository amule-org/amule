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

#include "GuiTestDiagnostics.h"
#include "TransferRateLabel.h"

#include <wx/wx.h>

#include <iostream>

class TransferRateLabelTestApp : public wxApp
{
public:
	bool OnInit() override { return true; }
};

wxIMPLEMENT_APP_NO_MAIN(TransferRateLabelTestApp);

namespace
{
void TestTransferRateLabel()
{
	wxDialog dialog(nullptr,
		wxID_ANY,
		"Transfer rate spacing test",
		wxDefaultPosition,
		wxDefaultSize,
		wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
	auto *rates = new TransferRateLabel(&dialog, wxID_ANY, "Up: 0.0 | Down: 0.0");
	auto *following = new wxStaticText(&dialog, wxID_ANY, "Connected");
	auto *strip = new wxBoxSizer(wxHORIZONTAL);
	strip->Add(rates, wxSizerFlags().Border(wxRIGHT, 5));
	strip->Add(following);
	dialog.SetSizer(strip);
	dialog.SetClientSize(dialog.FromDIP(wxSize(900, 100)));
	dialog.Show();
	wxTheApp->Yield();

	for (bool largeFont : { false, true }) {
		if (largeFont) {
			rates->SetFont(rates->GetFont().Scaled(1.5));
			// GTK applies font metrics on its next frame, after the change event.
			for (int frame = 0; frame < 10; ++frame) {
				wxMilliSleep(10);
				wxTheApp->Yield(true);
			}
		}
		rates->UpdateRate("Up: 123.4 KiB/s | Down: 567.8 MiB/s", false);
		dialog.SetClientSize(wxSize(strip->GetMinSize().x, dialog.FromDIP(100)));
		dialog.Layout();
		const int occupiedWidth = rates->GetSize().x;
		const int followingX = following->GetPosition().x;
		for (const auto *text : { "Up: 0.0 | Down: 0.0",
			     "Up: 1.0 KiB/s | Down: 0.0",
			     "Up: 0.0 | Down: 2.0 KiB/s",
			     "Up: 9.9 KiB/s | Down: 10.0 KiB/s",
			     "Up: 0.0 (0.0) | Down: 0.0 (0.0)",
			     "Up: 0.0 | Down: 0.0",
			     "Up: 0.0 | Down: 0.0" }) {
			rates->UpdateRate(text, false);
			Check(rates->GetLabel() == text, "Rate text did not update");
			Check(rates->GetSize().x == occupiedWidth, "Rate field shrank between samples");
			Check(following->GetPosition().x == followingX, "Following status field moved");
			// Another status field can independently relayout the same strip.
			dialog.Layout();
			Check(following->GetPosition().x == followingX, "Parent layout lost reserved width");
		}
		dialog.SetClientSize(dialog.FromDIP(wxSize(900, 100)));
		dialog.Layout();
		const wxString longer = "Up: 123.4 MiB/s (123.4) | Down: 567.8 MiB/s (567.8)";
		rates->UpdateRate(longer, false);
		std::cout << "Rate layout: font=" << rates->GetFont().GetPointSize()
			  << " previous=" << occupiedWidth << " expanded=" << rates->GetSize().x
			  << " client=" << dialog.GetClientSize().x << '\n';
		Check(rates->GetSize().x > occupiedWidth, "Rate field did not grow for larger values");
		auto *reference = new wxStaticText(&dialog, wxID_ANY, longer);
		reference->SetFont(rates->GetFont());
		Check(rates->GetSize().x >= reference->GetBestSize().x, "Rate text is clipped");
		reference->Destroy();
	}
}

class CountingStatusSizer : public wxBoxSizer
{
public:
	explicit CountingStatusSizer(int orientation)
	: wxBoxSizer(orientation)
	{
	}
	void RepositionChildren(const wxSize &minimum) override
	{
		++layouts;
		wxBoxSizer::RepositionChildren(minimum);
	}
	int layouts = 0;
};

class TestRateLabel : public TransferRateLabel
{
public:
	using TransferRateLabel::TransferRateLabel;
	wxSize GetDPI() const override { return dpi == wxDefaultSize ? TransferRateLabel::GetDPI() : dpi; }
	wxSize dpi = wxDefaultSize;
};

void SettleRateFont()
{
	// GTK applies a changed font on a subsequent display frame.
	for (int frame = 0; frame < 10; ++frame) {
		wxMilliSleep(10);
		wxTheApp->Yield(true);
	}
}

void TestTransferRateLifecycle()
{
	const wxString idle = "Up: 0.0 | Down: 0.0";
	const wxString active = "Up: 123.4 KiB/s | Down: 567.8 KiB/s";
	const wxString overhead = "Up: 123.4 KiB/s (12.3) | Down: 567.8 KiB/s (45.6)";
	int sizeChanges = 0;
	wxDialog dialog(nullptr, wxID_ANY, "Rate label lifecycle");
	auto *rates = new TestRateLabel(&dialog, wxID_ANY, idle);
	auto *following = new wxStaticText(&dialog, wxID_ANY, "Connected");
	auto *info = new wxStaticText(
		&dialog, wxID_ANY, "Log", wxDefaultPosition, wxSize(140, -1), wxST_NO_AUTORESIZE);
	auto *strip = new CountingStatusSizer(wxHORIZONTAL);
	strip->Add(info, wxSizerFlags(1).FixedMinSize());
	strip->Add(new wxStaticText(&dialog, wxID_ANY, "Users: 123"));
	strip->Add(rates);
	strip->Add(following);
	auto *outer = new CountingStatusSizer(wxVERTICAL);
	outer->Add(new wxPanel(&dialog), wxSizerFlags(1).Expand());
	outer->Add(strip, wxSizerFlags().Expand());
	dialog.SetSizer(outer);
	// The production control receives updates before and after the frame is shown.
	rates->UpdateRate(overhead, true);
	dialog.SetClientSize(wxSize(outer->GetMinSize().x + dialog.FromDIP(100), dialog.FromDIP(150)));
	dialog.Show();
	SettleRateFont();
	rates->UpdateRate(overhead, true);

	const wxSize reservedSize = rates->GetSize();
	rates->Bind(wxEVT_SIZE, [&sizeChanges, reservedSize](wxSizeEvent &event) {
		if (event.GetSize() != reservedSize) {
			++sizeChanges;
		}
		event.Skip();
	});
	const int stripLayouts = strip->layouts;
	const int outerLayouts = outer->layouts;
	const wxRect followingRect = following->GetRect();
	const wxRect infoRect = info->GetRect();
	for (const auto &text : { idle, active, idle, idle }) {
		rates->UpdateRate(text, true);
		Check(rates->GetLabel() == text, "A retained width prevented a text update");
		Check(sizeChanges == 0, "A sample temporarily resized the native rate control");
		Check(strip->layouts == stripLayouts, "A fitting sample needlessly relaid out the strip");
		Check(outer->layouts == outerLayouts, "A sample relaid out the main content");
		Check(following->GetRect() == followingRect && info->GetRect() == infoRect,
			"A fitting sample moved an adjacent status field");
	}

	const int overheadWidth = rates->GetSize().x;
	rates->UpdateRate(idle, false);
	Check(rates->GetSize().x < overheadWidth, "Disabling overhead retained its reserved width");
	Check(outer->layouts == outerLayouts, "A mode change relaid out the main content");
	const int compactWidth = rates->GetSize().x;
	rates->UpdateRate(overhead, true);
	Check(rates->GetSize().x > compactWidth, "Re-enabling overhead clipped its text");
	rates->UpdateRate(idle, false);
	Check(rates->GetSize().x == compactWidth, "Repeated mode changes accumulated empty space");

	const wxFont originalFont = rates->GetFont();
	rates->SetFont(originalFont.Scaled(1.5));
	SettleRateFont();
	rates->UpdateRate(idle, false);
	const int largerFontWidth = rates->GetSize().x;
	Check(largerFontWidth > compactWidth, "A larger font clipped unchanged rate text");
	Check(strip->GetSize().y >= rates->GetSize().y, "A larger font escaped the status strip");
	rates->SetFont(originalFont);
	SettleRateFont();
	rates->UpdateRate(idle, false);
	Check(rates->GetSize().x == compactWidth, "A smaller font retained the larger font's width");

	// Exercise DPI cache invalidation through the same virtual query as production.
	// This simulates scale changes; it is not native multi-monitor coverage.
	const wxSize originalDPI = rates->GetDPI();
	for (const wxSize &dpi : { originalDPI * 2, originalDPI }) {
		rates->UpdateRate(active, false);
		rates->dpi = dpi;
		rates->UpdateRate(idle, false);
		Check(rates->GetSize().x == compactWidth, "A DPI change retained obsolete pixel widths");
	}

	// A newly created window must not inherit another window's peak width.
	rates->UpdateRate(active, false);
	auto *fresh = new TransferRateLabel(&dialog, wxID_ANY, idle);
	fresh->UpdateRate(idle, false);
	Check(fresh->GetMinSize().x == compactWidth, "Rate width leaked into a new control");
	fresh->Destroy();
}

} // namespace

int main(int argc, char **argv)
{
	return RunGuiTest(argc, argv, "No GUI display available; skipping rate label tests", [] {
		TestTransferRateLabel();
		TestTransferRateLifecycle();
		std::cout << "Transfer rate label regression tests passed\n";
	});
}
