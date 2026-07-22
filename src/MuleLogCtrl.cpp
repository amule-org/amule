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

#include "MuleLogCtrl.h"

#include <wx/settings.h>

CMuleLogCtrl::CMuleLogCtrl(wxWindow *parent,
	wxWindowID id,
	const wxPoint &pos,
	const wxSize &size,
	long style,
	const wxString &name)
: wxStyledTextCtrl(parent, id, pos, size, style, name)
, m_inBatch(false)
, m_batchTailing(false)
, m_scrollPending(false)
, m_lastAutoScrollLine(-1)
{
	// Store text as UTF-8 so AppendText()'s wxString conversion and the byte
	// positions used for styling agree (GetLength() is a byte count).
	SetCodePage(wxSTC_CP_UTF8);

	// Look like a plain log pane, not a code editor: no line-number / symbol /
	// fold margins.
	for (int margin = 0; margin < 3; ++margin) {
		SetMarginWidth(margin, 0);
	}
	// Word-wrap long lines, as the old wxTE_RICH2 pane did, so nothing is clipped
	// off the right edge; with wrapping on there is no horizontal scrollbar to
	// show. (Wrapping is why AtBottom() and the tail-scroll reason in display
	// lines rather than document lines.)
	SetWrapMode(wxSTC_WRAP_WORD);
	SetUseHorizontalScrollBar(false);

	// Theme-aware colours (matches the old wxTE_RICH2, which used the system
	// window colours -- so dark themes keep working).
	StyleSetForeground(wxSTC_STYLE_DEFAULT, wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
	StyleSetBackground(wxSTC_STYLE_DEFAULT, wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
	wxFont guiFont = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
	StyleSetFont(wxSTC_STYLE_DEFAULT, guiFont);
	// Propagate the default style to all styles, then make critical lines bold.
	StyleClearAll();
	StyleSetBold(Style_Critical, true);

	SetReadOnly(true);
}

bool CMuleLogCtrl::AtBottom()
{
	// Compare in *display* lines: GetFirstVisibleLine()/LinesOnScreen() count
	// wrapped rows, while GetLineCount() counts document lines, so with wrapping
	// on the two must be reconciled. The total display-line count is the first
	// display row of the last doc line plus how many rows it wraps to. Generous
	// by one so "sitting at the end" always re-tails on append.
	const int lastDoc = GetLineCount() - 1;
	const int displayLines = VisibleFromDocLine(lastDoc) + WrapCount(lastDoc);
	return GetFirstVisibleLine() + LinesOnScreen() >= displayLines - 1;
}

void CMuleLogCtrl::ScrollToBottom()
{
	// Request only -- OnInternalIdle() is the sole scroller. Keeping every scroll
	// in one place stops the batch/live tail-scroll from racing the idle
	// re-scroll loop: that loop tells a manual scroll from an append by watching
	// the first-visible line, and a direct ScrollToEnd() here would move it and
	// be misread as the user scrolling -- which aborted the catch-up mid-load, so
	// switching to the log while it was still replaying landed short (issue #547,
	// @ghysler). Deferring also naturally waits until the pane is on screen.
	if (!IsShownOnScreen()) {
		// No reliable first-visible baseline while hidden; let the first scroll
		// after the pane appears run unconditionally.
		m_lastAutoScrollLine = -1;
	}
	m_scrollPending = true;
}

void CMuleLogCtrl::OnInternalIdle()
{
	wxStyledTextCtrl::OnInternalIdle();

	// Sole scroller for every tail-scroll (live line, batch, or deferred while
	// hidden). IsShownOnScreen() is only evaluated while a scroll is pending, so
	// the common idle path stays a single bool test; a scroll requested while the
	// pane was hidden simply waits here until it is shown.
	if (!m_scrollPending || !IsShownOnScreen()) {
		return;
	}

	// With word-wrap on, Scintilla lays out wrapped lines incrementally over
	// several idles, so a single ScrollToEnd() the moment the pane appears (or
	// while the log is still replaying) lands short -- against a display-line
	// count that does not yet include the still-unwrapped tail (issue #547,
	// reported by @ghysler with wrapped lines on a narrow window). Re-scroll each
	// idle until the position stops moving (wrap has settled at the true bottom).
	// Appends do not move the first-visible line, so if it has moved away from
	// where our last auto-scroll left it the user scrolled -- bail and reset, so
	// a manual scroll is never fought and a later return to the bottom re-tails.
	if (m_lastAutoScrollLine != -1 && GetFirstVisibleLine() != m_lastAutoScrollLine) {
		m_scrollPending = false;
		m_lastAutoScrollLine = -1;
		return;
	}
	ScrollToEnd();
	const int firstVisible = GetFirstVisibleLine();
	if (firstVisible == m_lastAutoScrollLine) {
		m_scrollPending = false; // stable: layout settled at the bottom
	}
	m_lastAutoScrollLine = firstVisible;
}

void CMuleLogCtrl::AppendLogLine(const wxString &line, bool critical)
{
	const bool tail = m_inBatch ? false : AtBottom();

	if (!m_inBatch) {
		SetReadOnly(false);
	}

	const int start = GetLength();
	AppendText(line);
	// Style the bytes just appended (StartStyling/SetStyling work on the style
	// buffer, not the text, so read-only state is irrelevant here).
	StartStyling(start);
	SetStyling(GetLength() - start, critical ? Style_Critical : Style_Normal);

	if (!m_inBatch) {
		SetReadOnly(true);
		if (tail) {
			ScrollToBottom();
		}
	}
}

void CMuleLogCtrl::ClearLog()
{
	SetReadOnly(false);
	ClearAll();
	SetReadOnly(true);
}

void CMuleLogCtrl::BeginBatch()
{
	// No Freeze()/Thaw(): Scintilla does not auto-scroll on append, so lines
	// added below the fold cause no repaint until the tail-scroll (requested by
	// EndBatch(), applied on the next idle). Freezing would only leave the scroll
	// extent stale at Thaw.
	m_batchTailing = AtBottom();
	m_inBatch = true;
	SetReadOnly(false);
}

void CMuleLogCtrl::EndBatch()
{
	SetReadOnly(true);
	m_inBatch = false;
	if (m_batchTailing) {
		ScrollToBottom();
	}
}

// File_checked_for_headers
