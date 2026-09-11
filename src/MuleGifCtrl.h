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

#ifndef MULEGIFCTRL_H
#define MULEGIFCTRL_H

#include <wx/control.h>
#include <wx/timer.h>

const int GIFTIMERID = 271283;

class MuleGIFDecoder;
class wxBitmap;

/**
 * A simple widget for displaying a gif animation, based on the animation classes by Julian Smart
 * and Guillermo Rodriguez Garcia but specialized for aMule's reduced requirements. Redrawing is
 * flicker-free, via wxBufferedPaintDC.
 *
 * Several things are hardcoded to keep it simple, though they are easy to change: the animation
 * loops until Stop() is called, the gif is assumed to be transparent, and Start() begins at the
 * first frame rather than continuing a stopped animation.
 */
class MuleGifCtrl : public wxControl
{
private:
	//! A pointer to the current gif-animation.
	MuleGIFDecoder *m_decoder;
	//! Timer used for the delay between each frame.
	wxTimer m_timer;
	//! Current frame.
	wxBitmap m_frame;

public:
	/**
	 * See the wxWindow class documentation for more information.
	 */
	MuleGifCtrl(wxWindow *parent,
		wxWindowID id,
		const wxPoint &pos = wxDefaultPosition,
		const wxSize &size = wxDefaultSize,
		long style = 0,
		const wxValidator &validator = wxDefaultValidator,
		const wxString &name = wxControlNameStr);

	virtual ~MuleGifCtrl();

	/**
	 * Loads a gif image from a char array of @a size bytes, returning whether it loaded.
	 *
	 * Sets the current animation and displays its first frame; any animation already loaded is
	 * unloaded and stopped. To convert an image into a form this can read, use hexdump -- see
	 * inetdownload.h for how to format the output.
	 */
	bool LoadData(const char *data, int size);

	/**
	 * Starts playing the animation, provided one is set and it is not a static image.
	 */
	void Start();

	/**
	 * Stops the animation.
	 */
	void Stop();

	/**
	 * The preferred size of the widget, which is the size of the animation.
	 */
	virtual wxSize GetBestSize();

private:
	/**
	 * Timer function that selects the next frame in an animation.
	 */
	void OnTimer(wxTimerEvent &event);

	/**
	 * Draws the current frame, changed in OnTimer(), through a wxBufferedPaintDC. That, plus
	 * catching the ERASE_BACKGROUND events, avoids flicker on redraws.
	 */
	void OnPaint(wxPaintEvent &event);

	/**
	 * Avoids flicker when redrawing.
	 */
	void OnErase(wxEraseEvent &WXUNUSED(event)) {}

	//! Enables the event functions OnErase(), OnTimer() and OnPaint().
	wxDECLARE_EVENT_TABLE();
};

#endif

// File_checked_for_headers
