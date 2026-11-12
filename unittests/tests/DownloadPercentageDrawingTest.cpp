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

#include <wx/wx.h>
#include <wx/dcmemory.h>
#include <stdexcept>
#include <cstring>
#include "DownloadPercentageDrawing.h"
#include "NativeListTest.h"

namespace
{
void Require(bool condition, const char *message)
{
	if (!condition) {
		throw std::runtime_error(message);
	}
}

class ScaledView : public wxWindow
{
public:
	ScaledView(wxWindow *parent, int scale)
	: wxWindow(parent, wxID_ANY)
	, m_scale(scale)
	{
	}
	wxSize GetDPI() const override { return wxSize(96 * m_scale, 96 * m_scale); }
	double GetDPIScaleFactor() const override { return m_scale; }

private:
	int m_scale;
};

int Ink(const wxImage &image, const wxRect &area)
{
	int count = 0;
	for (int y = area.y; y < area.GetBottom() + 1; ++y) {
		for (int x = area.x; x < area.GetRight() + 1; ++x) {
			if (image.GetRed(x, y) < 128) {
				++count;
			}
		}
	}
	return count;
}

void Check(ScaledView &view, int points, int width)
{
	view.SetFont(wxFontInfo(points).Family(wxFONTFAMILY_SWISS));
	const wxFont originalFont = wxFontInfo(8).Family(wxFONTFAMILY_ROMAN).Italic();
	const wxColour originalColour(123, 45, 67);
	const wxRect cell(25, 20, width, points * 4);
	const wxSize size(400, 180);
	wxBitmap actual(size), expected(size), normal(size);
	wxMemoryDC dc(actual), reference(expected), control(normal);
	for (wxDC *context : { &dc, &reference, &control }) {
		context->SetBackground(*wxWHITE_BRUSH);
		context->Clear();
	}
	dc.SetFont(originalFont);
	dc.SetTextForeground(originalColour);
	dc.SetBackgroundMode(wxSOLID);
	DrawDownloadPercentage(dc, cell, "100.0%", view, true);
	Require(dc.GetFont() == originalFont, "Percentage leaked its bold font into other cells");
	Require(dc.GetTextForeground() == originalColour, "Percentage leaked its text colour");
	Require(dc.GetBackgroundMode() == wxSOLID, "Percentage leaked its background mode");
	// Native centred label is an independent reference for size, weight and placement.
	reference.SetFont(points <= 12 ? view.GetFont().Bold() : view.GetFont());
	reference.SetTextForeground(*wxBLACK);
	reference.DrawLabel("100.0%", cell, wxALIGN_CENTER);
	control.SetFont(view.GetFont());
	control.SetTextForeground(*wxBLACK);
	control.DrawLabel("100.0%", cell, wxALIGN_CENTER);
	dc.SelectObject(wxNullBitmap);
	reference.SelectObject(wxNullBitmap);
	control.SelectObject(wxNullBitmap);
	const wxImage result = actual.ConvertToImage();
	const wxImage wanted = expected.ConvertToImage();
	Require(memcmp(result.GetData(), wanted.GetData(), size.x * size.y * 3) == 0,
		"Percentage does not match a centred label with the expected font weight");
	const int normalInk = Ink(normal.ConvertToImage(), cell);
	Require(points <= 12 ? Ink(result, cell) > normalInk : Ink(result, cell) == normalInk,
		"Only small percentage fonts should be made bold");

	// Every ordinary percentage needs a contrast outline, including small fonts.
	dc.SelectObject(actual);
	dc.SetBackground(wxBrush(wxColour(220, 220, 220)));
	dc.Clear();
	DrawDownloadPercentage(dc, cell, "99.9%", view, false);
	dc.SelectObject(wxNullBitmap);
	const wxImage outlined = actual.ConvertToImage();
	int whites = 0;
	int blacks = 0;
	for (int y = 0; y < size.y; ++y) {
		for (int x = 0; x < size.x; ++x) {
			const int red = outlined.GetRed(x, y);
			whites += red > 245;
			blacks += red < 30;
		}
	}
	Require(whites > 0, "Ordinary percentage needs white text");
	Require(blacks > 0, "Every ordinary percentage font size needs a black outline");

	// A narrow cell and a pre-existing clip must not paint into adjacent rows/columns.
	dc.SelectObject(actual);
	dc.SetBackground(*wxWHITE_BRUSH);
	dc.Clear();
	const wxRect clip(35, 30, 15, 15);
	dc.SetClippingRegion(clip);
	DrawDownloadPercentage(dc, wxRect(30, 25, 25, 25), "100.0%", view, false);
	wxRect restoredClip;
	dc.GetClippingBox(restoredClip);
	Require(restoredClip == clip, "Percentage must preserve the caller's clip");
	dc.DestroyClippingRegion();
	dc.SelectObject(wxNullBitmap);
	const wxImage clipped = actual.ConvertToImage();
	for (int y = 0; y < size.y; ++y) {
		for (int x = 0; x < size.x; ++x) {
			Require(clip.Contains(x, y) || clipped.GetRed(x, y) == 255,
				"Narrow percentage painted outside its clipping region");
		}
	}
}
} // namespace

void RunTests()
{
	wxFrame frame(nullptr, wxID_ANY, "Download percentage drawing");
	for (int scale : { 1, 2 }) {
		auto *view = new ScaledView(&frame, scale);
		for (int points : { 10, 12, 13, 16, 22 }) {
			for (int width : { 180, 300 }) {
				Check(*view, points, width);
			}
		}
		delete view;
	}
}
