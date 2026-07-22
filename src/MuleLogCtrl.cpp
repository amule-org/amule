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
{
	// Store text as UTF-8 so AppendText()'s wxString conversion and the byte
	// positions used for styling agree (GetLength() is a byte count).
	SetCodePage(wxSTC_CP_UTF8);

	// Look like a plain log pane, not a code editor: no line-number / symbol /
	// fold margins.
	for (int margin = 0; margin < 3; ++margin) {
		SetMarginWidth(margin, 0);
	}
	// Word-wrap long lines (as the old wxTE_RICH2 pane did) and drop the
	// horizontal scrollbar entirely. The h-scrollbar is not just cosmetic: on
	// the macOS and Windows Scintilla backends it is drawn inside the client
	// area but its height is NOT subtracted from LinesOnScreen(), so
	// ScrollToEnd() leaves the last line or two hidden behind it -- the log
	// looked stuck a few lines short of the bottom (issue #547). GTK draws it
	// outside the client area, which is why Linux was unaffected. With wrapping
	// on there is no horizontal scrollbar at all.
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
	if (!IsShownOnScreen()) {
		// The control's notebook page is hidden (e.g. the first-sync backlog
		// arrives while another tab is in front on launch), so it has no
		// laid-out geometry and ScrollToEnd() lands on a stale extent -- the log
		// then sits a few lines short once shown. Defer to NotifyShown()
		// (issue #547).
		m_scrollPending = true;
		return;
	}
	ScrollToEnd();
}

void CMuleLogCtrl::OnInternalIdle()
{
	wxStyledTextCtrl::OnInternalIdle();

	// Apply a tail-scroll that was deferred while the control was hidden, now
	// that its page/sub-tab is on screen and has real geometry. IsShownOnScreen()
	// is only evaluated while a scroll is actually pending, so the common
	// idle path stays a single bool test.
	if (m_scrollPending && IsShownOnScreen()) {
		m_scrollPending = false;
		ScrollToEnd();
	}
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
	// added below the fold cause no repaint until the single ScrollToBottom() in
	// EndBatch(). Freezing would only leave the scroll extent stale at Thaw.
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
