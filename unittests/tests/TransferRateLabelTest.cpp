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
#include <utility>

class TransferRateLabelTestApp : public wxApp
{
public:
	bool OnInit() override { return true; }
};

wxIMPLEMENT_APP_NO_MAIN(TransferRateLabelTestApp);

namespace
{
// Stands in for RelayoutLabelStrip(), which the main window calls on a true result.
void UpdateRate(TransferRateLabel *label, const wxString &text, bool showOverhead)
{
	if (label->UpdateRate(text, showOverhead)) {
		if (wxSizer *strip = label->GetContainingSizer()) {
			strip->Layout();
		}
	}
}

void TestRateFormattingAndAlignment()
{
	Check(TransferRateLabel::FormatRate(0, 0, false) == "0.0 KiB/s", "Idle rate lost its unit");
	Check(TransferRateLabel::FormatRate(1024, 0, false) == "1.0 KiB/s", "Small rate used the wrong unit");
	Check(TransferRateLabel::FormatRate(1024 * 1024 - 1, 0, false) == "1024.0 KiB/s",
		"Rate switched units below one MiB/s");
	Check(TransferRateLabel::FormatRate(1024 * 1024, 0, false) == "1.0 MiB/s",
		"Rate did not switch at one MiB/s");
	Check(TransferRateLabel::FormatRate(0, 1024, true) == "0.0 KiB/s (1.0)",
		"Idle rate or overhead lost its units/value");

	wxDialog dialog(nullptr, wxID_ANY, "Independent rate alignment");
	auto *strip = new wxBoxSizer(wxHORIZONTAL);
	auto *up = new wxStaticText(&dialog, wxID_ANY, "Up:");
	auto *upload = new TransferRateLabel(&dialog, wxID_ANY, TransferRateLabel::FormatRate(0, 0, false));
	auto *down = new wxStaticText(&dialog, wxID_ANY, "| Down:");
	auto *download = new TransferRateLabel(&dialog, wxID_ANY, TransferRateLabel::FormatRate(0, 0, false));
	auto *following = new wxStaticText(&dialog, wxID_ANY, "Connected");
	for (wxWindow *window : std::initializer_list<wxWindow *>{ up, upload, down, download, following }) {
		strip->Add(window, wxSizerFlags().Border(wxRIGHT, 5));
	}
	dialog.SetSizer(strip);
	UpdateRate(upload, TransferRateLabel::FormatRate(999 * 1024, 0, false), false);
	UpdateRate(download, TransferRateLabel::FormatRate(999 * 1024, 0, false), false);
	dialog.SetClientSize(strip->GetMinSize());
	dialog.Show();
	wxTheApp->Yield();
	const wxRect upRect = up->GetRect();
	const wxRect downRect = down->GetRect();
	const wxRect followingRect = following->GetRect();
	const wxRect uploadRect = upload->GetRect();
	const wxRect downloadRect = download->GetRect();
	Check((upload->GetWindowStyleFlag() & wxALIGN_RIGHT) != 0 &&
			(download->GetWindowStyleFlag() & wxALIGN_RIGHT) != 0,
		"Rate values are not right-aligned");
	for (const auto &sample : { std::pair<double, double>{ 0, 1024 }, { 1024 * 1024, 0 }, { 0, 0 } }) {
		UpdateRate(upload, TransferRateLabel::FormatRate(sample.first, 0, false), false);
		UpdateRate(download, TransferRateLabel::FormatRate(sample.second, 0, false), false);
		dialog.Layout();
		Check(up->GetRect() == upRect && down->GetRect() == downRect &&
				following->GetRect() == followingRect,
			"Independent rate updates moved a prefix or following status field");
		Check(upload->GetRect() == uploadRect && download->GetRect() == downloadRect,
			"Independent rate updates moved a value's right edge");
		Check(upload->GetTextExtent(upload->GetLabel()).x <= upload->GetSize().x &&
				download->GetTextExtent(download->GetLabel()).x <= download->GetSize().x,
			"A rate value was clipped");
	}
}

void TestTransferRateLabel()
{
	wxDialog dialog(nullptr,
		wxID_ANY,
		"Transfer rate spacing test",
		wxDefaultPosition,
		wxDefaultSize,
		wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
	auto *rates = new TransferRateLabel(&dialog, wxID_ANY, "0.0 KiB/s");
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
		UpdateRate(rates, "567.8 MiB/s", false);
		dialog.SetClientSize(wxSize(strip->GetMinSize().x, dialog.FromDIP(100)));
		dialog.Layout();
		const int occupiedWidth = rates->GetSize().x;
		const int followingX = following->GetPosition().x;
		for (double bytesPerSecond : { 0, 1024, 2048, 10240, 1024 * 1024, 0, 0 }) {
			const wxString text = TransferRateLabel::FormatRate(bytesPerSecond, 0, false);
			UpdateRate(rates, text, false);
			Check(rates->GetLabel() == text, "Rate text did not update");
			Check(rates->GetSize().x == occupiedWidth, "Rate field shrank between samples");
			Check(following->GetPosition().x == followingX, "Following status field moved");
			// Another status field can independently relayout the same strip.
			dialog.Layout();
			Check(following->GetPosition().x == followingX, "Parent layout lost reserved width");
		}
		dialog.SetClientSize(dialog.FromDIP(wxSize(900, 100)));
		dialog.Layout();
		const wxString longer = "567.8 MiB/s (567.8)";
		UpdateRate(rates, longer, false);
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

template <typename Base> class DpiTestLabel : public Base
{
public:
	using Base::Base;
	wxSize GetDPI() const override { return dpi == wxDefaultSize ? Base::GetDPI() : dpi; }
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
	const wxString idle = "0.0 KiB/s";
	const wxString active = "567.8 KiB/s";
	const wxString overhead = "567.8 KiB/s (45.6)";
	int sizeChanges = 0;
	wxDialog dialog(nullptr, wxID_ANY, "Rate label lifecycle");
	auto *rates = new DpiTestLabel<TransferRateLabel>(&dialog, wxID_ANY, idle);
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
	UpdateRate(rates, overhead, true);
	dialog.SetClientSize(wxSize(outer->GetMinSize().x + dialog.FromDIP(100), dialog.FromDIP(150)));
	dialog.Show();
	SettleRateFont();
	UpdateRate(rates, overhead, true);

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
		UpdateRate(rates, text, true);
		Check(rates->GetLabel() == text, "A retained width prevented a text update");
		Check(sizeChanges == 0, "A sample temporarily resized the native rate control");
		Check(strip->layouts == stripLayouts, "A fitting sample needlessly relaid out the strip");
		Check(outer->layouts == outerLayouts, "A sample relaid out the main content");
		Check(following->GetRect() == followingRect && info->GetRect() == infoRect,
			"A fitting sample moved an adjacent status field");
	}

	const int overheadWidth = rates->GetSize().x;
	UpdateRate(rates, idle, false);
	Check(rates->GetSize().x < overheadWidth, "Disabling overhead retained its reserved width");
	Check(outer->layouts == outerLayouts, "A mode change relaid out the main content");
	const int compactWidth = rates->GetSize().x;
	UpdateRate(rates, overhead, true);
	Check(rates->GetSize().x > compactWidth, "Re-enabling overhead clipped its text");
	UpdateRate(rates, idle, false);
	Check(rates->GetSize().x == compactWidth, "Repeated mode changes accumulated empty space");

	const wxFont originalFont = rates->GetFont();
	rates->SetFont(originalFont.Scaled(1.5));
	SettleRateFont();
	UpdateRate(rates, idle, false);
	const int largerFontWidth = rates->GetSize().x;
	Check(largerFontWidth > compactWidth, "A larger font clipped unchanged rate text");
	Check(strip->GetSize().y >= rates->GetSize().y, "A larger font escaped the status strip");
	rates->SetFont(originalFont);
	SettleRateFont();
	UpdateRate(rates, idle, false);
	Check(rates->GetSize().x == compactWidth, "A smaller font retained the larger font's width");

	// Exercise DPI cache invalidation through the same virtual query as production.
	// This simulates scale changes; it is not native multi-monitor coverage.
	const wxSize originalDPI = rates->GetDPI();
	// A DPI override also affects native font measurements on wxMSW. Compare
	// with a plain label at the same DPI, not the pixel width at the old DPI.
	auto *reference = new DpiTestLabel<wxStaticText>(&dialog, wxID_ANY, idle);
	reference->SetFont(rates->GetFont());
	for (const wxSize &dpi : { originalDPI * 2, originalDPI }) {
		UpdateRate(rates, active, false);
		const int retainedWidth = rates->GetMinSize().x;
		rates->dpi = dpi;
		reference->dpi = dpi;
		reference->InvalidateBestSize();
		const int expectedWidth = reference->GetBestSize().x;
		UpdateRate(rates, idle, false);
		std::cout << "DPI layout: dpi=" << dpi.x << " retained=" << retainedWidth
			  << " expected=" << expectedWidth << " actual=" << rates->GetSize().x << '\n';
		Check(rates->GetSize().x == expectedWidth, "A DPI change retained obsolete pixel widths");
		Check(rates->GetMinSize().x == expectedWidth, "A DPI change left an obsolete minimum");
	}
	reference->Destroy();

	// A newly created window must not inherit another window's peak width.
	UpdateRate(rates, active, false);
	auto *fresh = new TransferRateLabel(&dialog, wxID_ANY, idle);
	UpdateRate(fresh, idle, false);
	Check(fresh->GetMinSize().x == compactWidth, "Rate width leaked into a new control");
	fresh->Destroy();
}

} // namespace

int main(int argc, char **argv)
{
	return RunGuiTest(argc, argv, "No GUI display available; skipping rate label tests", [] {
		TestRateFormattingAndAlignment();
		TestTransferRateLabel();
		TestTransferRateLifecycle();
		std::cout << "Transfer rate label regression tests passed\n";
	});
}
