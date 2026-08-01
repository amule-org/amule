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

#ifndef SPLASHSCREEN_H
#define SPLASHSCREEN_H

#include <wx/bitmap.h> // Needed for the pre-rendered backdrop member
#include <wx/frame.h>
#include <wx/timer.h>

// Startup splash for the monolithic GUI.
//
// Exists because startup is slow and silent: the main window is created
// before the shared-file scan runs, but the scan holds the main thread, so
// nothing paints until it finishes. On a large share -- worse still on
// network storage, where every file costs a round trip instead of a page
// cache hit -- that is a long stretch with no sign the application is alive.
// Under Flatpak it is also long enough for xdg-desktop-portal's background
// monitor to conclude the app has no window and kill it.
//
// Everything is drawn by hand rather than assembled from child controls: the
// background is a dark gradient and native widgets on top of it would follow
// the desktop theme instead, giving a light-on-dark gauge on most systems.
class CSplashScreen : public wxFrame
{
public:
	CSplashScreen();

	// Update the phase text and the bar. Cheap to call often: repainting is
	// rate-limited internally, so callers can report every file scanned
	// without deciding for themselves how much is too much.
	//
	// Set @a immediate when the update marks a change of phase rather than
	// progress within one, to paint it without waiting out the rate limit.
	// It cannot be inferred from the text changing: the per-item updates
	// carry a running count, so their text changes every single time.
	void SetProgress(const wxString &status, int percent, bool immediate = false);

	// Tell the splash the main event loop is running again, so updates stop
	// pumping it themselves. Startup reaches this point when the blocking
	// phases are done and only the asynchronous hashing drain is left.
	void SetLoopRunning();

	// Close, honouring the minimum on-screen time. A fast startup would
	// otherwise flash the splash for a few frames, which reads as a glitch
	// rather than as the application starting.
	//
	// Never blocks: when the floor has not been reached it schedules the
	// close and returns. Sleeping here would freeze the main thread for up
	// to that floor, which is the very symptom the splash exists to explain
	// -- and on a fast startup it would do so for no reason at all.
	void Finish();

	// Stamps the clock the minimum-display floor is measured from, so that
	// floor covers the time actually spent visible rather than the time
	// since the object was built.
	bool Show(bool show = true) override;

private:
	void OnPaint(wxPaintEvent &evt);
	void OnCloseTimer(wxTimerEvent &evt);

	// Runs down the remainder of the minimum display time when Finish() is
	// called early, so that wait costs the main thread nothing.
	wxTimer m_closeTimer;

	// Gradient, logo and the two fixed captions, rendered once. Only the
	// bar and the phase text change afterwards, so the expensive part is
	// paid at construction rather than on every update.
	void RenderBackdrop();

	wxBitmap m_backdrop;
	wxString m_status;
	int m_percent;

	// Wall-clock of Show(), for the minimum-display-time floor.
	wxLongLong m_shownAt;
	// Wall-clock of the last repaint, for the rate limit.
	wxLongLong m_lastPaint;
	// Whether an update has to pump the event loop to reach the screen.
	bool m_pumpsLoop;

	// Cost accounting, reported once at Finish() so what the splash takes
	// from the work it is reporting on is measurable rather than assumed.
	// Two figures because they answer different questions: how much was
	// drawn, and how much time the startup path gave up in total (drawing
	// plus the event-loop pumping that gets it on screen).
	unsigned m_paintCount;
	wxLongLong m_paintMicros;
	wxLongLong m_updateMicros;

	wxDECLARE_EVENT_TABLE();
};

#endif // SPLASHSCREEN_H
