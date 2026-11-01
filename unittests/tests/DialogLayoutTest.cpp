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

#include "DialogLayout.h"
#include "FileRenameDialog.h"

#include <iostream>
#include <stdexcept>

class DialogLayoutTestApp : public wxApp
{
public:
	bool OnInit() override { return true; }
};

wxIMPLEMENT_APP_NO_MAIN(DialogLayoutTestApp);

namespace
{
void TestWrappingText(const wxString &message, bool scrolled, bool largerFont)
{
	wxEventLoop loop;
	wxEventLoopActivator active(&loop);
	wxDialog dialog(nullptr,
		wxID_ANY,
		"Wrapping label test",
		wxDefaultPosition,
		wxDefaultSize,
		wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
	wxWindow *content = scrolled ? static_cast<wxWindow *>(new wxScrolledWindow(&dialog))
				     : static_cast<wxWindow *>(new wxPanel(&dialog));
	content->SetMinSize(dialog.FromDIP(wxSize(20, 20)));
	if (scrolled) {
		ConfigureDialogScrolling(static_cast<wxScrolledWindow *>(content));
	}
	wxWindow *labelParent = scrolled ? new wxPanel(content) : content;
	auto *label = new WrappingStaticText(labelParent, wxID_ANY, message);
	// Read-only so keystrokes typed while a test dialog has focus cannot change it.
	auto *following = new wxTextCtrl(
		labelParent, wxID_ANY, "Keep this text", wxDefaultPosition, wxDefaultSize, wxTE_READONLY);
	if (scrolled) {
		// Preferences rows can keep the virtual page wider than the viewport.
		following->SetMinSize(dialog.FromDIP(wxSize(560, -1)));
	}
	auto *fields = new wxBoxSizer(wxVERTICAL);
	fields->Add(label, wxSizerFlags().Expand().Border(wxALL, 10));
	fields->Add(following, wxSizerFlags().Expand().Border(wxALL, 10));
	labelParent->SetSizer(fields);
	if (scrolled) {
		auto *pageSizer = new wxBoxSizer(wxVERTICAL);
		pageSizer->Add(labelParent, wxSizerFlags().Expand().Border(wxLEFT, 12));
		content->SetSizer(pageSizer);
	}
	auto *row = new wxBoxSizer(wxHORIZONTAL);
	row->Add(dialog.FromDIP(80), 1);
	row->Add(content, wxSizerFlags(1).Expand().Border(wxALL, 10));
	dialog.SetSizer(row);
	int callbacks = 0;
	label->SetLayoutCallback([&] {
		Check(++callbacks < 100, "Wrapping repeatedly schedules layout without settling");
		if (scrolled) {
			static_cast<wxScrolledWindow *>(content)->FitInside();
		} else {
			content->Layout();
		}
	});
	dialog.SetClientSize(dialog.FromDIP(wxSize(680, 500)));
	dialog.Show();
	wxTheApp->Yield();
	if (largerFont) {
		wxFont font = label->GetFont();
		font.SetPointSize(font.GetPointSize() + 4);
		label->SetFont(font);
	}
	wxClientDC dc(label);
	dc.SetFont(label->GetFont());
	// Wrap() measures the mnemonic-escaped label, including doubled ampersands.
	const int unwrappedWidth = dc.GetMultiLineTextExtent(wxControl::EscapeMnemonics(message)).x;
	const int displayWidth =
		dialog.ToDIP(dialog.WindowToClientSize(wxDisplay(&dialog).GetClientArea().GetSize())).x;
	const int wide = wxMin(displayWidth, wxMax(680, dialog.ToDIP(unwrappedWidth) + 160));
	auto resize = [&](int width) {
		dialog.SetClientSize(dialog.FromDIP(wxSize(width, 500)));
		dialog.SendSizeEvent();
		// Drain deferred layouts, including a scrollbar-induced size change.
#ifdef __WXGTK__
		// An X11 window manager answers each resize late, briefly restoring the previous
		// size, so wait until the new size holds.
		for (int attempt = 0, stable = 0; attempt < 300 && stable < 5; ++attempt) {
			wxMilliSleep(10);
			wxTheApp->Yield(true);
			stable =
				dialog.GetClientSize() == dialog.FromDIP(wxSize(width, 500)) ? stable + 1 : 0;
		}
#else
		for (int pass = 0; pass < 3; ++pass) {
			wxTheApp->Yield();
		}
#endif
		Check(dialog.GetClientSize() == dialog.FromDIP(wxSize(width, 500)),
			"Translation prevented resizing the dialog");
		label->InvalidateBestSize();
		Check(dc.GetMultiLineTextExtent(label->GetLabelText()).x <= label->GetClientSize().x,
			"Wrapped text is clipped horizontally");
		Check(label->GetBestSize().y <= label->GetClientSize().y,
			"Wrapped text is clipped vertically");
		Check(label->GetRect().GetBottom() < following->GetRect().GetTop(),
			"Wrapped text overlaps the next control");
		const int left =
			label->ClientToScreen(wxPoint(0, 0)).x - content->ClientToScreen(wxPoint(0, 0)).x;
		// wxOSX measures a wrapped line up to 1 px wider here than Wrap() did.
		Check(dc.GetMultiLineTextExtent(label->GetLabelText()).x <=
				content->GetClientSize().x - left + 1,
			"Hint extends past the visible viewport");
	};
	resize(400);
	const wxString narrow = label->GetLabelText();
	const int narrowHeight = label->GetSize().y;
	Check(narrow.Freq('\n') > message.Freq('\n'), "Narrow control did not wrap");
	const int settledCallbacks = callbacks;
	resize(400);
	Check(label->GetLabelText() == narrow && callbacks == settledCallbacks,
		"Same-width layout changed wrapping or scheduled another layout");
	if (scrolled) {
		const int virtualLabelWidth = label->GetSize().x;
		resize(340);
		Check(label->GetSize().x == virtualLabelWidth,
			"Viewport test must keep the label at the page's minimum content width");
		resize(400);
	}
	resize(wide);
	// A small desktop may not have enough room to display the whole translation.
	const int visibleWidth = wxMin(label->GetClientSize().x,
		content->GetClientSize().x -
			(label->ClientToScreen(wxPoint(0, 0)).x - content->ClientToScreen(wxPoint(0, 0)).x));
	if (unwrappedWidth <= visibleWidth) {
		Check(label->GetLabelText() == message,
			"Widening lost original text, ampersands or blank lines");
		Check(label->GetSize().y < narrowHeight, "Widening did not reclaim the wrapped height");
	}
	resize(400);
	if (label->GetLabelText() != narrow || label->GetSize().y != narrowHeight) {
		std::cerr << "Wrapping changed: scrolled=" << scrolled << ", largerFont=" << largerFont
			  << ", height=" << label->GetSize().y << ", previous=" << narrowHeight
			  << ", width=" << label->GetSize().x << '\n'
			  << narrow.ToUTF8().data() << "\n---\n"
			  << label->GetLabelText().ToUTF8().data() << '\n';
	}
	Check(label->GetLabelText() == narrow && label->GetSize().y == narrowHeight,
		"Repeated resizing accumulated line breaks or changed height");
	Check(label->GetMinSize().x == label->FromDIP(20), "Translation enlarged minimum width");
	if (following->GetValue() != "Keep this text") {
		std::cerr << "Neighboring content: " << following->GetValue().ToUTF8().data() << '\n';
	}
	Check(following->GetValue() == "Keep this text", "Wrapping changed neighboring content");
}

void TestWrappingControls()
{
	const wxString english = "Enter one server.met URL per line.\nSaving replaces the "
				 "source list and removes comment lines.";
	const wxString russian =
		wxString::FromUTF8("Введите по одному URL-адресу server.met на строку.\n"
				   "Сохранение заменяет список источников и удаляет строки комментариев.");
	const wxString literal = "Addresses & sources && URLs must remain readable when resized.\n\n"
				 "Keep intentional blank lines and a trailing newline.\n";
	for (bool scrolled : { false, true }) {
		for (bool largerFont : { false, true }) {
			for (const wxString &message : { english, russian, literal }) {
				TestWrappingText(message, scrolled, largerFont);
			}
		}
	}
	// A dialog can close before the deferred height adjustment is delivered.
	wxDialog dialog(nullptr, wxID_ANY, "Pending wrapping layout");
	auto *page = new wxScrolledWindow(&dialog);
	auto *label = new WrappingStaticText(page, wxID_ANY, english);
	bool called = false;
	label->SetLayoutCallback([&] { called = true; });
	label->SetSize(wxSize(100, 100));
	delete label;
	page->SetSize(wxSize(200, 200));
	page->SendSizeEvent();
	wxTheApp->ProcessPendingEvents();
	Check(!called, "Destroyed control retained a pending layout callback");
}

// Preferences measures each page right after building it, before the dialog is shown.
void TestWrappingMeasuredBeforeShown()
{
	wxDialog dialog(nullptr, wxID_ANY, "Wrapping before show");
	auto *page = new wxScrolledWindow(&dialog);
	ConfigureDialogScrolling(page);
	auto *label = new WrappingStaticText(page,
		wxID_ANY,
		"When the connected core's files live on a different machine, map a path prefix it "
		"reports to where that filesystem is reachable from here (a network share, say) so "
		"\"Open\" and \"Show in folder\" work.");
	auto *following = new wxTextCtrl(page, wxID_ANY, wxEmptyString);
	following->SetMinSize(dialog.FromDIP(wxSize(560, -1)));
	auto *fields = new wxBoxSizer(wxVERTICAL);
	fields->Add(label, wxSizerFlags().Expand());
	fields->Add(following, wxSizerFlags().Expand());
	page->SetSizer(fields);
	// The default size of a new window on wxOSX.
	page->SetSize(dialog.FromDIP(wxSize(20, 20)));
	page->FitInside();
	label->SendSizeEvent();
	page->SendSizeEvent();
	Check(label->GetLabelText().Freq('\n') < 4,
		"A page measured before showing wrapped its hint to the default page width");
}

// Drive the actual text control and button handlers, including wxWidgets' transfer
// of edited text on OK. EndModal alone would bypass that part of the dialog.
void TestFileRenameDialog()
{
	for (const wxString &current : { wxString("short.txt"), wxString('W', 300) }) {
		for (const int buttonID : { wxID_OK, wxID_CANCEL }) {
			wxString output = "unchanged";
			bool inspected = false;
			wxString failure;
			wxTheApp->CallAfter([&] {
				wxTextEntryDialog *dialog = nullptr;
				for (wxWindow *window : wxTopLevelWindows) {
					if (auto *candidate = dynamic_cast<wxTextEntryDialog *>(window)) {
						dialog = candidate;
						break;
					}
				}
				if (!dialog) {
					throw std::runtime_error("Rename dialog did not open");
				}
				// Close the modal loop even if an assertion fails in this callback.
				try {
					wxTextCtrl *text = nullptr;
					for (wxWindow *child : dialog->GetChildren()) {
						if (auto *candidate = dynamic_cast<wxTextCtrl *>(child)) {
							text = candidate;
							break;
						}
					}
					Check(text && text->GetValue() == current,
						"Rename lost the current filename");
					const wxSize fitted = dialog->GetSize();
					Check(dialog->GetMinHeight() == fitted.y &&
							dialog->GetMaxHeight() == fitted.y,
						"Rename dialog height is not fixed at its fitted size");
					Check(dialog->HasFlag(wxRESIZE_BORDER) &&
							dialog->GetMaxWidth() == wxDefaultCoord,
						"Rename dialog cannot be widened");
					dialog->SetSize(wxSize(fitted.x + 100, fitted.y));
					dialog->Layout();
					Check(dialog->GetSize().x > fitted.x &&
							dialog->GetSize().y == fitted.y,
						"Widening the rename dialog changed its height");
					text->SetValue("renamed.txt");
					inspected = true;
					wxCommandEvent click(wxEVT_BUTTON, buttonID);
					dialog->GetEventHandler()->ProcessEvent(click);
				} catch (const std::exception &error) {
					failure = wxString::FromUTF8(error.what());
					dialog->EndModal(wxID_CANCEL);
				}
			});
			const bool accepted = AskNewFileName(nullptr, CPath(current), output);
			Check(failure.empty(), failure.ToUTF8().data());
			Check(inspected, "Rename dialog was not inspected");
			Check(accepted == (buttonID == wxID_OK),
				"Rename acceptance does not match the button");
			Check(output == (accepted ? "renamed.txt" : "unchanged"),
				"Rename did not return edited text or modified output on cancellation");
		}
	}
}

void TestScrollableContent()
{
	wxDialog dialog(nullptr,
		wxID_ANY,
		"Dialog layout test",
		wxDefaultPosition,
		wxDefaultSize,
		wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
	wxScrolledWindow *content = new wxScrolledWindow(&dialog);
	content->SetMinSize(dialog.FromDIP(wxSize(240, 200)));
	ConfigureDialogScrolling(content);

	wxBoxSizer *fields = new wxBoxSizer(wxVERTICAL);
	wxStaticText *label = new wxStaticText(content, wxID_ANY, wxString('W', 200));
	fields->Add(label);
	wxPanel *section = new wxPanel(content);
	section->SetMinSize(dialog.FromDIP(wxSize(1200, 1200)));
	fields->Add(section);
	content->SetSizer(fields);

	wxButton *button = new wxButton(&dialog, wxID_OK);
	wxBoxSizer *top = new wxBoxSizer(wxVERTICAL);
	top->Add(content, wxSizerFlags(1).Expand());
	top->Add(button, wxSizerFlags().Right().Border(wxALL, 5));
	dialog.SetSizer(top);
	FitScrollableDialog(&dialog, content);
	std::vector<wxSize> sizes;
	UpdateDialogContentLayout(content, sizes);

	const wxSize available = wxDisplay(&dialog).GetClientArea().GetSize();
	Check(dialog.GetSize().x <= available.x && dialog.GetSize().y <= available.y,
		"Initial dialog extends beyond the display work area");

	const wxSize compact = dialog.FromDIP(wxSize(640, 480));
	dialog.SetSize(compact);
	dialog.Layout();
	content->FitInside();
	Check(dialog.GetSize() == compact, "Content prevents shrinking the dialog");
	Check(content->GetVirtualSize().x > content->GetClientSize().x &&
			content->GetVirtualSize().y > content->GetClientSize().y,
		"Oversized content is clipped instead of scrollable");
	Check(button->GetParent() == &dialog && button->GetRect().GetBottom() <= dialog.GetClientSize().y &&
			button->GetRect().GetRight() <= dialog.GetClientSize().x,
		"Action button is outside the visible dialog");

	content->Scroll(10, 10);
	int x = 0;
	int y = 0;
	content->GetViewStart(&x, &y);
	Check(x > 0 && y > 0, "Both scroll directions must work");

	// File Details changes both section visibility and label lengths on updates.
	section->Hide();
	label->SetLabel("Short name");
	Check(UpdateDialogContentLayout(content, sizes), "Removed content must refresh the layout");
	dialog.Layout();
	Check(dialog.GetSize() == compact, "Hiding a section resized the dialog");
	Check(content->GetVirtualSize().x <= content->GetClientSize().x &&
			content->GetVirtualSize().y <= content->GetClientSize().y,
		"Virtual size did not shrink after content was removed");

	section->Show();
	Check(UpdateDialogContentLayout(content, sizes), "Shown content must refresh the layout");
	dialog.Layout();
	Check(dialog.GetSize() == compact, "Showing a section resized the dialog");
	Check(content->GetVirtualSize().y >= fields->GetMinSize().y,
		"Newly shown content cannot be reached by scrolling");
}

void TestHorizontalScrollEdge()
{
	wxDialog dialog(nullptr, wxID_ANY, "Horizontal scroll edge test");
	wxScrolledWindow *content = new wxScrolledWindow(&dialog);
	ConfigureDialogScrolling(content);
	wxBoxSizer *fields = new wxBoxSizer(wxVERTICAL);
	wxPanel *section = new wxPanel(content);
	section->SetMinSize(wxSize(1003, 100));
	fields->Add(section, wxSizerFlags().Expand());
	content->SetSizer(fields);
	wxBoxSizer *top = new wxBoxSizer(wxVERTICAL);
	top->Add(content, wxSizerFlags(1).Expand());
	dialog.SetSizer(top);

	// Exercise every remainder of the old ten-pixel horizontal scroll unit.
	for (int width = 313; width < 323; ++width) {
		dialog.SetClientSize(wxSize(width, 300));
		dialog.Layout();
		content->FitInside();
		content->Scroll(100000, 0);
		int x = 0, y = 0, unitX = 0, unitY = 0;
		content->GetViewStart(&x, &y);
		content->GetScrollPixelsPerUnit(&unitX, &unitY);
		Check(x > 0, "Horizontal edge test must overflow the viewport");
		Check(x * unitX + content->GetClientSize().x == content->GetVirtualSize().x,
			"Horizontal scrolling exposes empty space beyond the content edge");
	}
}

void TestHeightCappedContent()
{
	wxDialog dialog(nullptr, wxID_ANY, "Tall dialog layout test");
	wxScrolledWindow *content = new wxScrolledWindow(&dialog);
	content->SetMinSize(wxSize(240, 200));
	ConfigureDialogScrolling(content);
	wxBoxSizer *fields = new wxBoxSizer(wxVERTICAL);
	wxPanel *section = new wxPanel(content);
	const wxSize available = wxDisplay(&dialog).GetClientArea().GetSize();
	section->SetMinSize(wxSize(400, available.y * 2));
	fields->Add(section, wxSizerFlags().Expand());
	content->SetSizer(fields);
	wxBoxSizer *top = new wxBoxSizer(wxVERTICAL);
	top->Add(content, wxSizerFlags(1).Expand());
	dialog.SetSizer(top);
	FitScrollableDialog(&dialog, content);
	dialog.Layout();
	content->FitInside();
	Check(content->GetVirtualSize().y > content->GetClientSize().y,
		"Tall content should scroll vertically");
	Check(content->GetVirtualSize().x <= content->GetClientSize().x,
		"Vertical scrollbar unnecessarily forces horizontal scrolling");
}

void TestWidthCappedContent()
{
	wxDialog dialog(nullptr, wxID_ANY, "Wide dialog layout test");
	wxScrolledWindow *content = new wxScrolledWindow(&dialog);
	content->SetMinSize(wxSize(240, 200));
	ConfigureDialogScrolling(content);
	wxBoxSizer *fields = new wxBoxSizer(wxVERTICAL);
	wxPanel *section = new wxPanel(content);
	const wxSize available = wxDisplay(&dialog).GetClientArea().GetSize();
	section->SetMinSize(wxSize(available.x * 2, 200));
	fields->Add(section, wxSizerFlags().Expand());
	content->SetSizer(fields);
	wxBoxSizer *top = new wxBoxSizer(wxVERTICAL);
	top->Add(content, wxSizerFlags(1).Expand());
	dialog.SetSizer(top);
	FitScrollableDialog(&dialog, content);
	dialog.Layout();
	content->FitInside();
	Check(content->GetVirtualSize().x > content->GetClientSize().x,
		"Wide content should scroll horizontally");
	Check(content->GetVirtualSize().y <= content->GetClientSize().y,
		"Horizontal scrollbar unnecessarily forces vertical scrolling");
}

// Preferences keeps one scrolled page per tab and is sized for the largest of
// them, which need not be the page shown while the dialog is being fitted.
void TestPagedContent()
{
	wxDialog dialog(nullptr, wxID_ANY, "Paged dialog layout test");
	wxPanel *sidebar = new wxPanel(&dialog);
	sidebar->SetMinSize(wxSize(150, 100));
	const wxSize minimumPageSize(240, 200);
	wxScrolledWindow *page = new wxScrolledWindow(&dialog);
	page->SetMinSize(minimumPageSize);
	ConfigureDialogScrolling(page);
	wxBoxSizer *fields = new wxBoxSizer(wxVERTICAL);
	wxPanel *section = new wxPanel(page);
	section->SetMinSize(wxSize(400, 100));
	fields->Add(section, wxSizerFlags().Expand());
	page->SetSizer(fields);
	wxBoxSizer *row = new wxBoxSizer(wxHORIZONTAL);
	row->Add(sidebar, wxSizerFlags().Expand());
	row->Add(page, wxSizerFlags(1).Expand());
	wxBoxSizer *top = new wxBoxSizer(wxVERTICAL);
	top->Add(row, wxSizerFlags(1).Expand());
	top->Add(new wxButton(&dialog, wxID_OK), wxSizerFlags().Right());
	dialog.SetSizer(top);
	page->FitInside();

	// The shown page is short; another page is taller than the display allows.
	const wxSize largest(400, wxDisplay(&dialog).GetClientArea().GetSize().y * 2);
	FitDialogToDisplay(&dialog,
		wxSize(largest.x + sidebar->GetMinSize().x,
			largest.y + top->GetMinSize().y - minimumPageSize.y));
	GrowDialogToContent(&dialog, page, largest);
	dialog.Layout();
	Check(page->GetVirtualSize().y <= page->GetClientSize().y,
		"Fitting left a stale vertical range on the shown page");

	// Switch to the tall page.
	section->SetMinSize(largest);
	page->FitInside();
	dialog.Layout();
	Check(page->GetVirtualSize().y > page->GetClientSize().y, "Tall page should scroll vertically");
	Check(page->GetVirtualSize().x <= page->GetClientSize().x,
		"Tall page's vertical scrollbar unnecessarily forces horizontal scrolling");
}

void TestChangingColumnsAndRename()
{
	wxDialog dialog(nullptr, wxID_ANY, "Changing columns layout test");
	wxScrolledWindow *content = new wxScrolledWindow(&dialog);
	ConfigureDialogScrolling(content);
	wxBoxSizer *fields = new wxBoxSizer(wxVERTICAL);
	wxPanel *fixed = new wxPanel(content);
	fixed->SetMinSize(wxSize(1000, 100));
	fields->Add(fixed);
	wxPanel *panel = new wxPanel(content);
	wxFlexGridSizer *grid = new wxFlexGridSizer(2, 0, 0);
	grid->AddGrowableCol(0);
	grid->AddGrowableCol(1);
	wxStaticText *value = new wxStaticText(panel, wxID_ANY, "1");
	wxStaticText *neighbor = new wxStaticText(panel, wxID_ANY, "Other column");
	grid->Add(value);
	grid->Add(neighbor);
	panel->SetSizer(grid);
	fields->Add(panel, wxSizerFlags().Expand());
	content->SetSizer(fields);
	wxBoxSizer *top = new wxBoxSizer(wxVERTICAL);
	top->Add(content, wxSizerFlags(1).Expand());
	dialog.SetSizer(top);
	dialog.SetClientSize(wxSize(1000, 400));
	dialog.Layout();
	std::vector<wxSize> sizes;
	Check(UpdateDialogContentLayout(content, sizes), "Initial content must be laid out");
	const wxSize minimum = fields->GetMinSize();
	const int oldColumnX = neighbor->GetPosition().x;
	// Choose a label wider than its allocated column but smaller than the fixed section.
	wxString label;
	while (value->GetTextExtent(label).x <= oldColumnX + 20) {
		label += 'W';
	}
	value->SetLabel(label);
	Check(fields->GetMinSize() == minimum, "Test must keep the overall minimum unchanged");
	Check(UpdateDialogContentLayout(content, sizes), "Changed columns must trigger layout");
	Check(neighbor->GetPosition().x > value->GetRect().GetRight(),
		"A growing value overlaps its neighboring column");
	Check(!UpdateDialogContentLayout(content, sizes), "Unchanged content must skip relayout");

	// Apply updates the filename immediately rather than waiting for the next timer tick.
	value->SetLabel(wxString('W', 200));
	UpdateDialogContentLayout(content, sizes);
	Check(content->GetVirtualSize().x >= fields->GetMinSize().x,
		"Renamed content is unreachable until the next timer update");
	Check(dialog.GetClientSize() == wxSize(1000, 400), "Content update resized the dialog");
	value->SetLabel("Short name");
	UpdateDialogContentLayout(content, sizes);
	Check(content->GetVirtualSize().x <= content->GetClientSize().x,
		"Shorter renamed content leaves a stale horizontal range");
}

void TestSmallContent()
{
	wxDialog dialog(nullptr, wxID_ANY, "Small dialog layout test");
	wxScrolledWindow *content = new wxScrolledWindow(&dialog);
	content->SetMinSize(dialog.FromDIP(wxSize(240, 200)));
	ConfigureDialogScrolling(content);
	wxBoxSizer *fields = new wxBoxSizer(wxVERTICAL);
	fields->Add(new wxStaticText(content, wxID_ANY, "A small setting"));
	content->SetSizer(fields);
	wxBoxSizer *top = new wxBoxSizer(wxVERTICAL);
	top->Add(content, wxSizerFlags(1).Expand());
	top->Add(new wxButton(&dialog, wxID_OK), wxSizerFlags().Right());
	dialog.SetSizer(top);
	FitScrollableDialog(&dialog, content);
	dialog.Layout();
	Check(dialog.GetSize().x >= dialog.GetMinSize().x && dialog.GetSize().y >= dialog.GetMinSize().y,
		"Initial size ignores the fixed controls' minimum");
	Check(content->GetClientSize().x >= fields->GetMinSize().x &&
			content->GetClientSize().y >= fields->GetMinSize().y,
		"Small content should fit without scrolling");
}
} // namespace

int main(int argc, char **argv)
{
	// GUI tests need a display (for example xvfb-run on Linux CI). Do not report
	// success when the platform cannot initialize its GUI backend.
	return RunGuiTest(argc, argv, "No GUI display available; skipping dialog layout tests", [] {
		TestWrappingControls();
		TestWrappingMeasuredBeforeShown();
		TestFileRenameDialog();
		TestScrollableContent();
		TestSmallContent();
		TestHeightCappedContent();
		TestWidthCappedContent();
		TestPagedContent();
		TestChangingColumnsAndRename();
		TestHorizontalScrollEdge();
		std::cout << "Dialog layout regression tests passed\n";
	});
}
