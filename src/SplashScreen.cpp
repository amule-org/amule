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

#include "SplashScreen.h"

#include <algorithm> // Needed for std::min / std::max
#include <cmath>     // Needed for std::sqrt

#include <wx/artprov.h>
#include <wx/dcbuffer.h>
#include <wx/image.h>
#include <wx/utils.h> // Needed for wxSafeYield

#include <common/Format.h> // Needed for CFormat

#include <common/ClientVersion.h> // Needed for VERSION_MJR and friends

#include "CamuleArtProvider.h" // Needed for CamuleArtProvider::MakeId
#include "Logger.h"

namespace
{

// Splash geometry, in DIPs. Wide enough for the tagline at a readable size
// -- it is the longest line and sets the floor.
constexpr int kSplashWidth = 440;
constexpr int kSplashHeight = 340;
constexpr int kLogoSize = 128;

// Gradient, matching the artwork the splash replaces: an ellipse centred on
// the panel, falling off linearly from a lifted grey to black. The radius is
// normalised per axis, so the ellipse follows the panel's shape rather than
// being circular, and it reaches black past the corners (which sit at
// r = sqrt(2) ~ 1.41) rather than exactly at them.
constexpr int kCentreR = 79;
constexpr int kCentreG = 79;
constexpr int kCentreB = 82;
constexpr double kBlackAtRadius = 1.39;

// Repaint at most this often while the caller reports progress. Time-based
// rather than every-N-items: the shared-file scan runs at wildly different
// speeds depending on whether the files are local or on network storage, so
// a count-based rule is either too coarse or too costly depending on the
// share. At 10 Hz the bar looks continuous and the cost stays negligible
// against work measured in seconds.
constexpr long kRepaintIntervalMs = 100;

// Minimum time on screen. A startup that beats this would otherwise show
// the splash for a couple of frames, which looks like a flicker.
constexpr long kMinimumVisibleMs = 1500;

// Shrinks @a font until @a text fits @a maxWidth, and draws it centred.
//
// A guard rather than a layout scheme: every string here is short enough at
// the intended size, but they are translated and the panel is fixed-width, so
// one long translation would otherwise run off both edges unnoticed.
void DrawFittedText(
	wxDC &dc, const wxString &text, wxFont font, int maxWidth, int centreX, int y, wxSize &extent)
{
	for (;;) {
		dc.SetFont(font);
		extent = dc.GetTextExtent(text);
		if (extent.x <= maxWidth || font.GetPointSize() <= 6) {
			break;
		}
		font.SetPointSize(font.GetPointSize() - 1);
	}
	dc.DrawText(text, centreX - extent.x / 2, y);
}

wxColour GradientAt(double nx, double ny)
{
	const double r = std::sqrt(nx * nx + ny * ny);
	const double k = std::max(0.0, 1.0 - r / kBlackAtRadius);
	return wxColour(static_cast<unsigned char>(kCentreR * k),
		static_cast<unsigned char>(kCentreG * k),
		static_cast<unsigned char>(kCentreB * k));
}

} // namespace

wxBEGIN_EVENT_TABLE(CSplashScreen, wxFrame)
	EVT_PAINT(CSplashScreen::OnPaint)
	EVT_TIMER(wxID_ANY, CSplashScreen::OnCloseTimer)
wxEND_EVENT_TABLE()

CSplashScreen::CSplashScreen()
: wxFrame(nullptr,
	  wxID_ANY,
	  wxEmptyString,
	  wxDefaultPosition,
	  wxDefaultSize,
	  wxFRAME_NO_TASKBAR | wxSTAY_ON_TOP | wxBORDER_NONE)
, m_percent(0)
, m_shownAt(0)
, m_lastPaint(0)
, m_pumpsLoop(true)
, m_paintCount(0)
, m_paintMicros(0)
, m_updateMicros(0)
{
	SetClientSize(FromDIP(wxSize(kSplashWidth, kSplashHeight)));
	SetBackgroundStyle(wxBG_STYLE_PAINT);
	RenderBackdrop();
	Centre();
}

bool CSplashScreen::Show(bool show)
{
	const bool shown = wxFrame::Show(show);
	if (show && m_shownAt == 0) {
		m_shownAt = wxGetUTCTimeMillis();
	}
	return shown;
}

void CSplashScreen::RenderBackdrop()
{
	const wxSize size = GetClientSize();
	wxImage gradient(size.x, size.y);

	const double halfW = size.x / 2.0;
	const double halfH = size.y / 2.0;
	for (int y = 0; y < size.y; ++y) {
		const double ny = (y + 0.5 - halfH) / halfH;
		for (int x = 0; x < size.x; ++x) {
			const double nx = (x + 0.5 - halfW) / halfW;
			const wxColour c = GradientAt(nx, ny);
			gradient.SetRGB(x, y, c.Red(), c.Green(), c.Blue());
		}
	}

	m_backdrop = wxBitmap(gradient);

	wxMemoryDC dc(m_backdrop);
	dc.SetTextForeground(*wxWHITE);

	const int logoSize = FromDIP(kLogoSize);
	const wxBitmap logo = wxArtProvider::GetBitmap(
		CamuleArtProvider::MakeId("splash_logo"), wxART_OTHER, wxSize(logoSize, logoSize));
	int y = FromDIP(28);
	if (logo.IsOk()) {
		dc.DrawBitmap(logo, (size.x - logo.GetWidth()) / 2, y, true);
		y += logo.GetHeight() + FromDIP(12);
	}

	// Application name and running version. The version-number macros rather
	// than GetMuleVersion(): that one deliberately describes the build for
	// debugging -- toolkit, Boost, snapshot revision -- which is a paragraph,
	// not a title.
	wxFont title = GetFont();
	title.SetPointSize(title.GetPointSize() + 6);
	title.SetWeight(wxFONTWEIGHT_BOLD);
	const wxString name = wxString::Format("aMule v%d.%d.%d", VERSION_MJR, VERSION_MIN, VERSION_UPDATE);
	const int textWidth = size.x - 2 * FromDIP(24);
	wxSize extent;
	DrawFittedText(dc, name, title, textWidth, size.x / 2, y, extent);
	y += extent.y + FromDIP(6);

	// Same string the About dialog uses, minus its trailing whitespace.
	dc.SetTextForeground(wxColour(190, 190, 195));
	wxString tagline = _("'All-Platform' p2p client based on eMule \n\n");
	tagline.Trim(true).Trim(false);
	DrawFittedText(dc, tagline, GetFont(), textWidth, size.x / 2, y, extent);
}

void CSplashScreen::OnPaint(wxPaintEvent &WXUNUSED(evt))
{
	const wxLongLong started = wxGetUTCTimeMillis();
	wxAutoBufferedPaintDC dc(this);
	dc.DrawBitmap(m_backdrop, 0, 0, false);

	const wxSize size = GetClientSize();
	const int margin = FromDIP(40);
	const int barHeight = FromDIP(10);
	const int barWidth = size.x - 2 * margin;
	const int barTop = size.y - FromDIP(64);

	// Trough, then fill. Drawn rather than delegated to wxGauge so the bar
	// keeps these colours on a dark panel instead of the theme's.
	dc.SetPen(wxPen(wxColour(90, 90, 95)));
	dc.SetBrush(wxBrush(wxColour(30, 30, 33)));
	dc.DrawRoundedRectangle(margin, barTop, barWidth, barHeight, barHeight / 2.0);

	const int clamped = std::min(100, std::max(0, m_percent));
	const int fill = (barWidth * clamped) / 100;
	if (fill > 0) {
		dc.SetPen(*wxTRANSPARENT_PEN);
		dc.SetBrush(wxBrush(wxColour(232, 138, 40))); // the mule's orange
		dc.DrawRoundedRectangle(margin, barTop, fill, barHeight, barHeight / 2.0);
	}

	dc.SetFont(GetFont());
	dc.SetTextForeground(wxColour(210, 210, 215));
	const wxSize extent = dc.GetTextExtent(m_status);
	dc.DrawText(m_status, (size.x - extent.x) / 2, barTop + barHeight + FromDIP(10));

	++m_paintCount;
	m_paintMicros += (wxGetUTCTimeMillis() - started);
}

void CSplashScreen::SetProgress(const wxString &status, int percent, bool immediate)
{
	m_status = status;
	m_percent = percent;

	const wxLongLong now = wxGetUTCTimeMillis();
	if (!immediate && (now - m_lastPaint) < kRepaintIntervalMs) {
		return;
	}
	m_lastPaint = now;

	Refresh(false);

	if (!m_pumpsLoop) {
		// The loop is running on its own and will pick the invalidation up
		// on the next frame. Yielding from here would nest a second loop
		// inside a handler dispatched by the first, and the work it ran
		// would be charged to the splash rather than to what is actually
		// doing it.
		m_updateMicros += (wxGetUTCTimeMillis() - now);
		return;
	}

	// The event loop has to run for the invalidation to reach the screen:
	// under GTK3, Update() cannot force a synchronous repaint the way it
	// does elsewhere -- drawing is driven by the frame clock, which only
	// ticks from the loop. Without this the bar simply does not move, which
	// is the whole point of showing it.
	//
	// wxSafeYield rather than wxYield: it disables every other top-level
	// window for the duration, so input that arrives while the application
	// is still half-built is discarded rather than dispatched into
	// subsystems that do not exist yet. The rate limit above bounds what
	// this costs; Finish() logs the total so it can be checked rather than
	// assumed.
	wxSafeYield(this, true);

	m_updateMicros += (wxGetUTCTimeMillis() - now);
}

void CSplashScreen::SetLoopRunning()
{
	m_pumpsLoop = false;
}

void CSplashScreen::Finish()
{
	const wxLongLong visibleMs = wxGetUTCTimeMillis() - m_shownAt;

	// Debug level: this is the splash's own overhead against the work it
	// reports on, which matters when tuning the phase weighting but is noise
	// in a user's log. The phase timings themselves are logged normally,
	// since those are what a "startup is slow" report needs.
	AddDebugLogLineN(logStandard,
		CFormat("Splash: %u repaints costing %lld ms, %lld ms taken from startup, "
			"over %lld ms on screen") %
			m_paintCount % m_paintMicros.GetValue() % m_updateMicros.GetValue() %
			visibleMs.GetValue());

	if (visibleMs >= kMinimumVisibleMs) {
		Hide();
		Destroy();
		return;
	}

	// Under the floor: hand the close to a one-shot timer so the main thread
	// carries on. wxWindow::Destroy() is already deferred (it posts to the
	// idle handler), so the frame outlives this call either way.
	const int remainingMs = static_cast<int>((kMinimumVisibleMs - visibleMs).ToLong());
	m_closeTimer.SetOwner(this);
	m_closeTimer.StartOnce(remainingMs);
}

void CSplashScreen::OnCloseTimer(wxTimerEvent &WXUNUSED(evt))
{
	Hide();
	Destroy();
}
