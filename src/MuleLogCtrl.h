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

#ifndef MULELOGCTRL_H
#define MULELOGCTRL_H

#include <wx/stc/stc.h>

// A read-only, append-only log view backed by wxStyledTextCtrl (Scintilla).
//
// Replaces the wxTE_RICH2 log panes (aMule Log, aMuleGUI Log, server info).
// RichEdit holds the whole document and reflows O(n) on scroll, so the tens of
// thousands of lines a remote-GUI first sync can dump made scrolling crawl and
// left the view mispainted (only the tail visible until a manual scroll) --
// issues #445, #547. Scintilla renders only the visible lines, so scroll and
// full-history retention stay O(visible) at any size, and unlike a virtual
// list it keeps character-level selection and find.
class CMuleLogCtrl : public wxStyledTextCtrl
{
public:
	CMuleLogCtrl(wxWindow *parent,
		wxWindowID id = wxID_ANY,
		const wxPoint &pos = wxDefaultPosition,
		const wxSize &size = wxDefaultSize,
		long style = 0,
		const wxString &name = "CMuleLogCtrl");

	// Append one already-newline-terminated line. `critical` renders it bold
	// (errors/warnings). Auto-scrolls to the end only when the view was already
	// at the bottom, so a user who scrolled up to read history is left in place.
	void AppendLogLine(const wxString &line, bool critical = false);

	// Remove all text.
	void ClearLog();

	// Bracket a burst of AppendLogLine() calls (one stats poll's backlog): the
	// control is left writable for the whole run, and the single tail-scroll is
	// deferred to EndBatch(), decided by whether the view was at the bottom when
	// the batch began.
	void BeginBatch();
	void EndBatch();

	// Sole scroller for every tail-scroll. ScrollToBottom() only flags one as
	// pending; this applies it -- waiting until the pane is on screen (a hidden
	// control cannot be scrolled reliably), and re-applying across idles until
	// the layout (which Scintilla wraps incrementally) settles at the true
	// bottom. Overriding at this level means every log/info pane inherits it.
	void OnInternalIdle() override;

private:
	bool AtBottom();
	void ScrollToBottom();

	// Scintilla style ids: 0 is the default text style, 1 adds bold.
	enum
	{
		Style_Normal = 0,
		Style_Critical = 1
	};

	bool m_inBatch;
	bool m_batchTailing;
	// A tail-scroll has been requested; OnInternalIdle() applies it (deferring
	// while the pane is hidden, and re-applying until the layout settles).
	bool m_scrollPending;
	// First-visible line left by the last auto-scroll while m_scrollPending is
	// being resolved, so the idle loop can tell when the (possibly wrapping)
	// layout has settled and when the user has scrolled away. -1 = none yet.
	int m_lastAutoScrollLine;
};

#endif // MULELOGCTRL_H
