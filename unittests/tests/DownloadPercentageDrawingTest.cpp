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

#include "DownloadPercentageDrawing.h"
#include "GuiTestDiagnostics.h"

#include <wx/dcmemory.h>
#include <wx/wx.h>

#include <functional>
#include <iostream>

class DownloadPercentageDrawingTestApp : public wxApp
{
public:
	bool OnInit() override { return true; }
};

wxIMPLEMENT_APP_NO_MAIN(DownloadPercentageDrawingTestApp);

namespace
{
const wxSize Canvas(160, 60);
const wxRect Cell(20, 15, 120, 30);

wxImage Paint(const wxColour &background, const std::function<void(wxDC &)> &draw)
{
	wxBitmap bitmap(Canvas);
	{
		wxMemoryDC dc(bitmap);
		dc.SetBackground(wxBrush(background));
		dc.Clear();
		draw(dc);
	}
	return bitmap.ConvertToImage();
}

int CountPixels(const wxImage &image, const std::function<bool(unsigned char)> &match)
{
	int count = 0;
	for (int y = 0; y < image.GetHeight(); ++y) {
		for (int x = 0; x < image.GetWidth(); ++x) {
			count += match(image.GetRed(x, y)) ? 1 : 0;
		}
	}
	return count;
}

void TestDrawingStateIsRestored(const wxWindow &view)
{
	const wxFont font = wxFontInfo(8).Italic();
	const wxColour colour(1, 2, 3);
	const wxRect clip(0, 0, Canvas.x - 1, Canvas.y - 1);
	Paint(*wxWHITE, [&](wxDC &dc) {
		dc.SetFont(font);
		dc.SetTextForeground(colour);
		dc.SetBackgroundMode(wxSOLID);
		dc.SetClippingRegion(clip);
		DrawDownloadPercentage(dc, Cell, "42.0%", view, false);
		DrawDownloadPercentage(dc, Cell, "42.0%", view, true);
		wxRect box;
		dc.GetClippingBox(box);
		Check(dc.GetFont() == font && dc.GetTextForeground() == colour &&
				dc.GetBackgroundMode() == wxSOLID && box == clip,
			"Percentage leaked its drawing state into later cells");
	});
}

void TestTextIsBoldAndOutlined(const wxWindow &view)
{
	auto dark = [](unsigned char red) { return red < 128; };
	// Hashing text has no outline, so its ink shows the weight alone.
	const wxImage hashing =
		Paint(*wxWHITE, [&](wxDC &dc) { DrawDownloadPercentage(dc, Cell, "42.0%", view, true); });
	const wxImage regular = Paint(*wxWHITE, [&](wxDC &dc) {
		dc.SetFont(view.GetFont());
		dc.SetTextForeground(*wxBLACK);
		dc.DrawLabel("42.0%", Cell, wxALIGN_CENTER);
	});
	Check(CountPixels(hashing, dark) > CountPixels(regular, dark), "Percentage is not bold");

	const wxImage ordinary = Paint(wxColour(128, 128, 128),
		[&](wxDC &dc) { DrawDownloadPercentage(dc, Cell, "42.0%", view, false); });
	Check(CountPixels(ordinary, [](unsigned char red) { return red > 230; }) > 0,
		"Ordinary percentage is not white");
	Check(CountPixels(ordinary, [](unsigned char red) { return red < 30; }) > 0,
		"Ordinary percentage has no outline");
}

void TestTextStaysInItsCell(const wxWindow &view)
{
	const wxRect narrow(60, 20, 12, 12);
	const wxImage image =
		Paint(*wxWHITE, [&](wxDC &dc) { DrawDownloadPercentage(dc, narrow, "100.0%", view, false); });
	for (int y = 0; y < Canvas.y; ++y) {
		for (int x = 0; x < Canvas.x; ++x) {
			Check(narrow.Contains(x, y) || image.GetRed(x, y) == 255,
				"Percentage painted outside its cell");
		}
	}
}
} // namespace

int main(int argc, char **argv)
{
	return RunGuiTest(argc, argv, "No GUI display available; skipping percentage tests", [] {
		wxFrame view(nullptr, wxID_ANY, "Download percentage drawing");
		TestDrawingStateIsRestored(view);
		TestTextIsBoldAndOutlined(view);
		TestTextStaysInItsCell(view);
		std::cout << "Download percentage drawing tests passed\n";
	});
}
