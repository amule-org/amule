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

#include <wx/wx.h>
#include "KadContactHistogram.h"
#include <iostream>
#include <stdexcept>
#include <cstdlib>

class HistogramTestApp : public wxApp
{
public:
	bool OnInit() override { return true; }
};
wxIMPLEMENT_APP_NO_MAIN(HistogramTestApp);

struct KadHistogramTestAccess
{
	static void CheckLongWord(CKadContactHistogram &panel, const wxString &text)
	{
		CKadContactHistogram::SummaryWrapper wrapped;
		const int width = panel.FromDIP(120);
		wrapped.Wrap(&panel, text, width);
		wxClientDC dc(&panel);
		dc.SetFont(panel.GetFont());
		wxString restored;
		for (const auto &line : wrapped.lines) {
			if (dc.GetTextExtent(line).x > width) {
				throw std::runtime_error("Long word clips horizontally");
			}
			restored += line;
		}
		if (wrapped.lines.size() < 2 || restored != text) {
			throw std::runtime_error("Word wrapping loses Unicode characters");
		}
	}
	static void CheckSummaryFits(CKadContactHistogram &panel)
	{
		CKadContactHistogram::SummaryWrapper wrapped;
		const int width = panel.GetClientSize().x - panel.FromDIP(16);
		wrapped.Wrap(&panel, panel.SummaryText(), width);
		wxClientDC dc(&panel);
		dc.SetFont(panel.GetFont());
		for (const auto &line : wrapped.lines) {
			if (dc.GetTextExtent(line).x > width) {
				throw std::runtime_error("Summary text clips horizontally");
			}
		}
		const int numericMargin = std::max(panel.FromDIP(36),
			dc.GetTextExtent(wxString::Format("%u", panel.m_data.Total())).x + panel.FromDIP(6));
		const int plotWidth = panel.GetClientSize().x - numericMargin - panel.FromDIP(8);
		if (plotWidth < dc.GetTextExtent("000").x + dc.GetTextExtent("fff").x) {
			throw std::runtime_error("KadID axis endpoint labels overlap");
		}
	}
	static void CheckTextOnlyMinimum(CKadContactHistogram &panel)
	{
		CKadContactHistogram::SummaryWrapper wrapped;
		wrapped.Wrap(&panel,
			panel.SummaryText(),
			std::max(1,
				std::max(panel.FromDIP(200), panel.GetClientSize().x) - panel.FromDIP(16)));
		wxClientDC dc(&panel);
		dc.SetFont(panel.GetFont());
		const int expected =
			panel.FromDIP(16) + static_cast<int>(wrapped.lines.size()) * dc.GetCharHeight();
		if (panel.GetMinSize().y != expected) {
			throw std::runtime_error("Non-chart state reserves space beyond its wrapped text");
		}
	}
	static wxImage Render(CKadContactHistogram &panel, const wxSize &size)
	{
		// Test sizes describe DIP layouts, while this bitmap/DC uses pixels.
		const wxSize pixels = panel.FromDIP(size);
		return RenderPixels(panel, pixels);
	}
	static wxImage RenderPixels(CKadContactHistogram &panel, const wxSize &pixels)
	{
		wxBitmap bitmap(pixels.x, pixels.y, 24);
		wxMemoryDC dc(bitmap);
		panel.Draw(dc, pixels);
		dc.SelectObject(wxNullBitmap);
		return bitmap.ConvertToImage();
	}
};

static void Check(bool condition, const char *message)
{
	if (!condition) {
		throw std::runtime_error(message);
	}
}
static size_t Pixels(const wxImage &image, const wxColour &colour)
{
	size_t count = 0;
	for (int y = 0; y < image.GetHeight(); ++y) {
		for (int x = 0; x < image.GetWidth(); ++x) {
			if (image.GetRed(x, y) == colour.Red() && image.GetGreen(x, y) == colour.Green() &&
				image.GetBlue(x, y) == colour.Blue()) {
				++count;
			}
		}
	}
	return count;
}
static size_t ColourRuns(const wxImage &image, const wxColour &colour)
{
	size_t runs = 0;
	bool previous = false;
	for (int x = 0; x < image.GetWidth(); ++x) {
		bool found = false;
		for (int y = 0; y < image.GetHeight(); ++y) {
			if (image.GetRed(x, y) == colour.Red() && image.GetGreen(x, y) == colour.Green() &&
				image.GetBlue(x, y) == colour.Blue()) {
				found = true;
				break;
			}
		}
		if (found && !previous) {
			++runs;
		}
		previous = found;
	}
	return runs;
}
static void SaveRender(const wxImage &image, const char *name)
{
	const char *directory = std::getenv("AMULE_HISTOGRAM_RENDER_DIR");
	if (directory) {
		Check(image.SaveFile(wxString::FromUTF8(directory) + "/" + name, wxBITMAP_TYPE_PNG),
			"Cannot save render artifact");
	}
}
static void VerifyRendering()
{
	wxFrame frame(nullptr, wxID_ANY, "Kad histogram test");
	CKadContactHistogram panel(&frame);
	panel.SetBackgroundColour(*wxWHITE);
	panel.SetForegroundColour(*wxBLACK);
	Kademlia::ContactDistribution populated;
	populated.contacts.fill(1000);
	populated.verified.fill(500);
	populated.subnets = 250;
	populated.hasLocalID = true;
	populated.localID = 0x80000000;
	panel.SetDistribution(populated, Kademlia::ContactDistributionState::Available);
	for (const wxSize size : { wxSize(240, 240), wxSize(740, 240) }) {
		const auto image = KadHistogramTestAccess::Render(panel, size);
		SaveRender(image, size.x < 300 ? "narrow-light.png" : "wide-light.png");
		Check(Pixels(image, wxColour(70, 130, 210)) > 0, "Total-contact bars missing");
		Check(Pixels(image, wxColour(35, 160, 90)) > 0, "Verified-contact bars missing");
	}
	// Dark GTK palette, with the native widget font and foreground/background.
	panel.SetBackgroundColour(wxColour(32, 32, 32));
	panel.SetForegroundColour(wxColour(240, 240, 240));
	const auto dark = KadHistogramTestAccess::Render(panel, wxSize(740, 240));
	SaveRender(dark, "wide-dark.png");
	Check(Pixels(dark, wxColour(70, 130, 210)) > 0, "Dark theme total bars missing");
	Check(Pixels(dark, wxColour(240, 240, 240)) > 0, "Dark theme marker/text missing");
	panel.SetDistribution({}, Kademlia::ContactDistributionState::Loading);
	Check(Pixels(KadHistogramTestAccess::Render(panel, wxSize(240, 240)), wxColour(70, 130, 210)) == 0,
		"Loading displays stale bars");
	// Two distinct bins within one former six-bit bin must remain separated at wide widths.
	Kademlia::ContactDistribution clustered;
	clustered.contacts[2048] = 10;
	clustered.contacts[2100] = 20;
	panel.SetDistribution(clustered, Kademlia::ContactDistributionState::Available);
	const auto clusteredImage = KadHistogramTestAccess::Render(panel, wxSize(740, 240));
	Check(ColourRuns(clusteredImage, wxColour(70, 130, 210)) == 2,
		"Distinct bins within one former six-bit bin collapse at wide widths");
	clustered.subnets = 2;
	clustered.verified[2048] = 5;
	clustered.verified[2100] = 10;
	clustered.hasLocalID = true;
	clustered.localID = 0x82000000;
	panel.SetDistribution(clustered, Kademlia::ContactDistributionState::Available);
	const auto markedImage = KadHistogramTestAccess::Render(panel, wxSize(740, 240));
	Check(Pixels(markedImage, wxColour(240, 240, 240)) >
			Pixels(clusteredImage, wxColour(240, 240, 240)) + 50,
		"Local KadID marker is missing or has insufficient contrast");
	SaveRender(markedImage, "clustered-dark.png");
	panel.SetDistribution(populated, Kademlia::ContactDistributionState::Available);
	const wxFont originalFont = panel.GetFont();
	wxFont font = originalFont;
	font.SetPointSize(24);
	panel.SetFont(font);
	KadHistogramTestAccess::CheckLongWord(panel, "KadContactDistributionABCDEFGHIJKLMN0123456789");
	KadHistogramTestAccess::CheckLongWord(panel, wxString::FromUTF8("非常长的联系人分布图说明"));
	// Allocate space relative to the enlarged font, including GTK font-DPI
	// scaling which does not necessarily change FromDIP() on this backend.
	const wxSize largeSize(panel.GetCharWidth() * 70, panel.GetCharHeight() * 8);
	const auto largeFont = KadHistogramTestAccess::Render(panel, largeSize);
	SaveRender(largeFont, "large-font.png");
	Check(Pixels(largeFont, wxColour(35, 160, 90)) > 0, "Large fonts hide the chart");
	// Exercise the Kad graph/chart proportions in a real sizer.
	auto *layout = new wxBoxSizer(wxVERTICAL);
	wxPanel graph(&frame);
	layout->Add(&graph, 3, wxEXPAND);
	layout->Add(&panel, 0, wxEXPAND);
	frame.SetSizer(layout);
	frame.SetClientSize(wxSize(std::max(panel.FromDIP(260), panel.GetMinSize().x), panel.FromDIP(1200)));
	panel.SetDistribution({}, Kademlia::ContactDistributionState::Loading);
	Check(layout->GetItem(&panel)->GetProportion() == 0,
		"Loading chart still takes a proportional share of the Kad pane");
	frame.Layout();
	panel.SetDistribution(populated, Kademlia::ContactDistributionState::Available);
	Check(layout->GetItem(&panel)->GetProportion() == 2,
		"Available chart does not share the Kad pane with the graph");
	wxTheApp->ProcessPendingEvents();
	frame.Layout();
	KadHistogramTestAccess::CheckSummaryFits(panel);
	Check(panel.GetSize().x <= frame.GetClientSize().x && panel.GetSize().y <= frame.GetClientSize().y,
		"Actual chart extends beyond its layout viewport");
	const auto actualLayout = KadHistogramTestAccess::RenderPixels(panel, panel.GetClientSize());
	SaveRender(actualLayout, "large-font-layout.png");
	Check(Pixels(actualLayout, wxColour(35, 160, 90)) > 0,
		"Actual Kad sizer height hides the chart with large fonts at narrow widths");
	const int expandedHeight = panel.GetMinSize().y;
	panel.SetFont(originalFont);
	wxTheApp->ProcessPendingEvents();
	frame.Layout();
	Check(panel.GetMinSize().y < expandedHeight, "Chart keeps stale large-font height");
	Check(Pixels(KadHistogramTestAccess::RenderPixels(panel, panel.GetClientSize()),
		      wxColour(35, 160, 90)) > 0,
		"Restoring the font hides the chart");
	frame.SetClientSize(wxSize(panel.FromDIP(380), panel.FromDIP(310)));
	frame.Layout();
	Check(graph.GetSize().y > panel.FromDIP(100),
		"Chart squeezes the Kad graph at a scaled window height");
	Check(panel.GetSize().y >= panel.GetMinSize().y, "Chart is smaller than its text and plot minimum");
	// Degenerate widths and heights must clip safely rather than divide by zero.
	KadHistogramTestAccess::Render(panel, wxSize(20, 20));
	panel.SetDistribution(populated, Kademlia::ContactDistributionState::Unsupported);
	wxTheApp->ProcessPendingEvents();
	frame.Layout();
	Check(layout->GetItem(&panel)->GetProportion() == 0,
		"Unsupported chart still takes a proportional share of the Kad pane");
	Check(panel.GetSize().y == panel.GetMinSize().y, "Unsupported chart uses more than its text height");
	KadHistogramTestAccess::CheckTextOnlyMinimum(panel);
	auto unavailable = KadHistogramTestAccess::Render(panel, wxSize(240, 240));
	Check(Pixels(unavailable, wxColour(70, 130, 210)) == 0, "Unavailable data displays stale bars");
	panel.SetDistribution(populated, Kademlia::ContactDistributionState::Invalid);
	Check(layout->GetItem(&panel)->GetProportion() == 0,
		"Invalid chart still takes a proportional share of the Kad pane");
	Check(Pixels(KadHistogramTestAccess::Render(panel, wxSize(240, 240)), wxColour(70, 130, 210)) == 0,
		"Invalid data displays stale bars");
	panel.SetDistribution({}, Kademlia::ContactDistributionState::Available);
	auto empty = KadHistogramTestAccess::Render(panel, wxSize(740, 360));
	Check(Pixels(empty, wxColour(35, 160, 90)) == 0, "Stopped Kad displays stale verified contacts");
}
int main(int argc, char **argv)
{
	wxInitAllImageHandlers();
	if (!wxEntryStart(argc, argv)) {
		return 77;
	}
	int result = 0;
	if (!wxTheApp->CallOnInit()) {
		result = 1;
	} else {
		try {
			VerifyRendering();
			std::cout << "Histogram render checks passed\n";
		} catch (const std::exception &error) {
			std::cerr << error.what() << '\n';
			result = 1;
		}
		wxTheApp->OnExit();
	}
	wxEntryCleanup();
	return result;
}
