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
#include "SearchQueryDialog.h"
#include <wx/wx.h>
#include <wx/spinctrl.h>
#include <wx/evtloop.h>
#include <wx/dcclient.h>
#include <wx/dcmemory.h>
#include <iostream>
#include <stdexcept>

class SearchQueryTestApp : public wxApp
{
public:
	bool OnInit() override { return true; }
};
wxIMPLEMENT_APP_NO_MAIN(SearchQueryTestApp);

namespace
{
void Check(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}
wxTextCtrl *Input(CSearchQueryDialog &dialog, const wxString &name)
{
	auto *field = dynamic_cast<wxTextCtrl *>(wxWindow::FindWindowByName(name, &dialog));
	Check(field != nullptr, "Missing preview input");
	return field;
}
void TestDialog(const wxString &screenshot)
{
	wxEventLoop loop;
	wxEventLoopActivator active(&loop);
	CSearchQueryDialog dialog(
		nullptr, "jazz artist:\"Miles Davis\" album:\"Kind of Blue\" duration:>=5m bitrate:>=192");
	dialog.Show();
	wxYield();
	CSearchQueryPreview result;
	Check(!dialog.ReadPreview(result), "Unreviewed input accepted");
	Check(dialog.PreviewQuery() && dialog.ReadPreview(result), "Valid preview rejected");
	Check(result.keywords == "jazz" && result.metadata.album == "Kind of Blue" &&
			result.metadata.minLength == 300,
		"Preview mapping wrong");
	wxYield();
	if (!screenshot.empty()) {
		// Allow GTK to finish painting the newly enabled Apply button for capture.
		wxMilliSleep(100);
		wxYield();
		wxInitAllImageHandlers();
		dialog.Update();
		wxClientDC source(&dialog);
		wxBitmap bitmap(dialog.GetClientSize());
		wxMemoryDC destination(bitmap);
		destination.Blit(0, 0, bitmap.GetWidth(), bitmap.GetHeight(), &source, 0, 0);
		destination.SelectObject(wxNullBitmap);
		Check(bitmap.SaveFile(screenshot, wxBITMAP_TYPE_PNG), "Could not save preview screenshot");
	}
	Input(dialog, _("Preview keywords"))->SetValue("edited jazz");
	Input(dialog, _("Artist"))->SetValue("Edited Artist");
	auto *duration = dynamic_cast<wxSpinCtrl *>(
		wxWindow::FindWindowByName(_("Minimum duration (seconds)"), &dialog));
	Check(duration != nullptr, "Missing duration control");
	duration->SetValue(600);
	Check(dialog.ReadPreview(result) && result.keywords == "edited jazz" &&
			result.metadata.artist == "Edited Artist" && result.metadata.minLength == 600,
		"Manual edits discarded");
	wxCommandEvent enter(wxEVT_TEXT_ENTER, Input(dialog, _("Artist"))->GetId());
	enter.SetEventObject(Input(dialog, _("Artist")));
	Input(dialog, _("Artist"))->GetEventHandler()->ProcessEvent(enter);
	Check(dialog.ReadPreview(result) && result.metadata.artist == "Edited Artist",
		"Enter discarded preview edits");
	Input(dialog, _("Preview keywords"))->SetValue(" ");
	Check(!dialog.ReadPreview(result), "Empty edited keywords accepted");
	Input(dialog, _("Query to translate"))->SetValue("rock artist:Queen");
	Check(!dialog.ReadPreview(result), "Stale preview accepted after query edit");
	Check(dialog.PreviewQuery() && dialog.ReadPreview(result) && result.metadata.album.empty() &&
			result.metadata.minLength == 0,
		"Re-preview retained old filters");
	Input(dialog, _("Query to translate"))->SetValue("rock bitrate:<100");
	Check(!dialog.PreviewQuery() && !dialog.ReadPreview(result), "Failed translation enabled Apply");
	wxYield();
	Check(dialog.GetClientRect().Contains(dialog.FindWindow(wxID_OK)->GetRect()),
		"Validation error clipped the preview action buttons");
	Check(Input(dialog, _("Artist"))->GetValue() == "Queen",
		"Failed translation erased editable preview");
	wxCommandEvent cancel(wxEVT_BUTTON, wxID_CANCEL);
	dialog.GetEventHandler()->ProcessEvent(cancel);
	Check(!dialog.IsShown(), "Cancel did not dismiss preview");
	CSearchQueryDialog invalidModal(nullptr, "rock bitrate:bad");
	bool invalidStayedOpen = false;
	wxTheApp->CallAfter([&] {
		invalidModal.PreviewQuery();
		wxCommandEvent accept(wxEVT_BUTTON, wxID_OK);
		invalidModal.GetEventHandler()->ProcessEvent(accept);
		invalidStayedOpen = invalidModal.IsModal();
		if (invalidStayedOpen)
			invalidModal.EndModal(wxID_CANCEL);
	});
	Check(invalidModal.ShowModal() == wxID_CANCEL && invalidStayedOpen,
		"Dialog-level OK bypassed preview validation");
	CSearchQueryDialog modal(nullptr, "rock artist:Queen");
	bool prepared = false;
	wxTheApp->CallAfter([&] {
		prepared = modal.PreviewQuery();
		Input(modal, _("Preview keywords"))->SetValue("edited rock");
		wxWindow *button = modal.FindWindow(prepared ? wxID_OK : wxID_CANCEL);
		wxCommandEvent accept(wxEVT_BUTTON, button->GetId());
		accept.SetEventObject(button);
		button->GetEventHandler()->ProcessEvent(accept);
	});
	Check(modal.ShowModal() == wxID_OK && prepared, "Use preview did not accept the modal dialog");
	Check(modal.ReadPreview(result) && result.keywords == "edited rock" &&
			result.metadata.artist == "Queen",
		"Modal result lost manual edits");
}
} // namespace

int main(int argc, char **argv)
{
	const wxString screenshot = argc > 1 ? wxString::FromUTF8(argv[1]) : wxString();
	InstallGuiTestDiagnostics();
	if (!wxEntryStart(argc, argv))
		return 77;
	RestoreGuiTestLog();
	int result = 0;
	if (!wxTheApp->CallOnInit())
		result = 1;
	else {
		try {
			TestDialog(screenshot);
			std::cout << "PASS: preview mapping, manual edits, validation, stale preview and "
				     "cancellation\n";
		} catch (const std::exception &error) {
			std::cerr << error.what() << '\n';
			result = 1;
		}
		wxTheApp->OnExit();
	}
	wxEntryCleanup();
	return result;
}
