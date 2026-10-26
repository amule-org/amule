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

#include <wx/wx.h>
#include <wx/evtloop.h>

#include "EditServerListDlg.h"
#include "DialogLayout.h"
#include "Preferences.h"

#include <iostream>
#include <stdexcept>

// The preferences entry point shares the production translation unit with the
// dialog. Layout tests must never access or modify application preferences.
wxString CPreferences::s_configDir;
wxArrayString CPreferences::GetServerListURLs()
{
	throw std::runtime_error("Layout test unexpectedly read server sources");
}
bool CPreferences::SetServerListURLs(const wxArrayString &)
{
	throw std::runtime_error("Layout test unexpectedly saved server sources");
}

class EditServerListTestApp : public wxApp
{
public:
	bool OnInit() override { return true; }
};

wxIMPLEMENT_APP_NO_MAIN(EditServerListTestApp);

namespace
{
void Check(bool condition, const char *message)
{
	if (!condition) {
		throw std::runtime_error(message);
	}
}

template <typename T> T *FindNestedControl(wxWindow &window)
{
	for (wxWindow *child : window.GetChildren()) {
		if (T *control = dynamic_cast<T *>(child)) {
			return control;
		}
		if (T *control = FindNestedControl<T>(*child)) {
			return control;
		}
	}
	return nullptr;
}

template <typename T> T *FindControl(wxWindow &window)
{
	if (T *control = FindNestedControl<T>(window)) {
		return control;
	}
	throw std::runtime_error("Missing dialog control");
}

void Resize(wxDialog &dialog, int width, int height)
{
	dialog.SetClientSize(dialog.FromDIP(wxSize(width, height)));
	dialog.SendSizeEvent();
	for (int pass = 0; pass < 3; ++pass) {
		wxTheApp->Yield();
	}
}

void CheckButtonsVisible(wxDialog &dialog)
{
	const wxRect client(wxPoint(0, 0), dialog.GetClientSize());
	for (int id : { wxID_OK, wxID_CANCEL }) {
		wxWindow *button = dialog.FindWindow(id);
		Check(button && client.Contains(button->GetRect()), "Dialog button is clipped");
	}
}

void TestWrappingControlIntegration()
{
	const wxString message = "Enter one server.met URL per line.\nSaving replaces the "
				 "source list and removes comment lines.";
	EditServerListDlg dialog(nullptr, "Server sources layout test", message, wxEmptyString);
	auto *label = FindControl<WrappingStaticText>(dialog);
	auto *editor = FindControl<wxTextCtrl>(dialog);
	dialog.Show();
	wxTheApp->Yield();
	const wxString urls = "https://example.org/server.met?one=1&two=2\nhttps://example.net/list";
	dialog.SetText(urls);
	editor->SetSelection(3, 12);
	for (int width : { 280, 900, 280 }) {
		Resize(dialog, width, 300);
		Check(label->GetRect().GetBottom() < editor->GetRect().GetTop(),
			"Wrapping control did not relayout the server source editor");
		CheckButtonsVisible(dialog);
	}
	Check(dialog.GetText() == urls, "Resizing changed the URL edits");
#ifndef __WXGTK__
	// wxGTK sometimes drops a text view's selection when it is resized (about 1 run in 20
	// under Xvfb), without recreating the control.
	long start = 0, end = 0;
	editor->GetSelection(&start, &end);
	Check(start == 3 && end == 12, "Resizing changed the editor selection");
#endif

	// Too short for the wrapped instructions and the editor: scroll, keep the editor usable.
	Resize(dialog, 200, 140);
	auto *content = dynamic_cast<wxScrolledWindow *>(editor->GetParent());
	Check(content != nullptr, "Server source editor is not in a scrolled area");
	Check(editor->GetSize().y >= editor->GetMinSize().y,
		"Short dialog squeezed the server source editor");
	Check(label->GetRect().GetBottom() < editor->GetRect().GetTop(),
		"Short dialog overlapped the instructions and the editor");
	Check(content->GetVirtualSize().y > content->GetClientSize().y,
		"Short dialog did not make the instructions and editor scrollable");
	CheckButtonsVisible(dialog);
	Resize(dialog, 280, 300);
	Check(dialog.GetText() == urls, "Scrolling layout changed the URL edits");
}
} // namespace

int main(int argc, char **argv)
{
	InstallGuiTestDiagnostics();
	if (!wxEntryStart(argc, argv)) {
		std::cerr << "No GUI display available; skipping server sources layout tests\n";
		return 77;
	}
	RestoreGuiTestLog();
	int result = 0;
	if (!wxTheApp->CallOnInit()) {
		result = 1;
	} else {
		try {
			wxEventLoop loop;
			wxEventLoopActivator active(&loop);
			TestWrappingControlIntegration();
			std::cout << "Server sources layout regression tests passed\n";
		} catch (const std::exception &error) {
			std::cerr << error.what() << '\n';
			result = 1;
		}
		wxTheApp->OnExit();
	}
	wxEntryCleanup();
	return result;
}
