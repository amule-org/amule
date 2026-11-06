// Copyright (c) 2026 aMule Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "GuiTestDiagnostics.h"

#include <wx/wx.h>
#include <wx/evtloop.h>

#include "TextClearButton.h"

#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <cmath>

class ClearButtonTestApp : public wxApp
{
public:
	bool OnInit() override { return true; }
};
wxIMPLEMENT_APP_NO_MAIN(ClearButtonTestApp);

namespace
{
void Require(bool condition, const char *message)
{
	if (!condition) {
		throw std::runtime_error(message);
	}
}

// Exercise field sizing and artwork with normal and high-DPI text controls.
class DpiTextCtrl : public wxTextCtrl
{
public:
	DpiTextCtrl(wxWindow *parent, double scale)
	: wxTextCtrl(parent, wxID_ANY)
	, m_scale(scale)
	{
	}
	wxSize GetDPI() const override { return wxSize(96 * m_scale, 96 * m_scale); }
	double GetDPIScaleFactor() const override { return m_scale; }

private:
	double m_scale;
};

class TextCounter : public wxEvtHandler
{
public:
	int count = 0;
	void OnText(wxCommandEvent &event)
	{
		++count;
		event.Skip();
	}
};

void CheckArtwork(const wxBitmap &bitmap, wxSystemColour colourId)
{
	Require(bitmap.IsOk(), "Clear artwork must be available");
	const auto bundle = CamuleArtProvider::GetMenuBitmapBundle(
		"menu_x_lg", wxSize(16, 16), wxSystemSettings::GetColour(colourId));
	const auto expected = bundle.GetBitmap(bitmap.GetSize()).ConvertToImage();
	const auto actual = bitmap.ConvertToImage();
	Require(actual.HasAlpha() && expected.HasAlpha(), "SVG artwork must retain transparency");
	int visiblePixels = 0;
	for (int y = 0; y < actual.GetHeight(); ++y) {
		for (int x = 0; x < actual.GetWidth(); ++x) {
			Require(std::abs(int(actual.GetAlpha(x, y)) - int(expected.GetAlpha(x, y))) <= 1,
				"Clear artwork must use the menu_x_lg silhouette");
			if (actual.GetAlpha(x, y) >= 200) {
				++visiblePixels;
				const int redError =
					std::abs(int(actual.GetRed(x, y)) - int(expected.GetRed(x, y)));
				const int greenError =
					std::abs(int(actual.GetGreen(x, y)) - int(expected.GetGreen(x, y)));
				const int blueError =
					std::abs(int(actual.GetBlue(x, y)) - int(expected.GetBlue(x, y)));
				Require(redError <= 3 && greenError <= 3 && blueError <= 3,
					"Clear artwork must use the current theme colour");
			}
		}
	}
	Require(visiblePixels > 0, "Clear cross must be visible");
}

void CheckRow(wxWindow *field, wxSizer *outer)
{
	auto *parent = field->GetParent();
	const auto id = field->GetId();
	auto *entry = dynamic_cast<wxTextEntry *>(field);
	// Compare against master's label-and-field row using the native metrics.
	auto *label = new wxStaticText(parent, wxID_ANY, "Filter:");
	auto *baseline = new wxBoxSizer(wxHORIZONTAL);
	baseline->Add(label, wxSizerFlags().CenterVertical());
	baseline->Add(field, wxSizerFlags().CenterVertical());
	const int baselineHeight = baseline->GetMinSize().y;
	baseline->SetDimension(0, 0, 600, baselineHeight);
	const int baselineLabelOffset = label->GetPosition().y - field->GetPosition().y;
	baseline->Detach(label);
	baseline->Detach(field);
	delete baseline;

	auto *row = WithTextClearButton(field);
	auto *labelledRow = new wxBoxSizer(wxHORIZONTAL);
	labelledRow->Add(label, wxSizerFlags().CenterVertical());
	labelledRow->Add(row, wxSizerFlags(1).CenterVertical());
	outer->Add(labelledRow, wxSizerFlags().Expand());
	outer->Layout();
	Require(labelledRow->GetMinSize().y == baselineHeight,
		"Clear button must preserve master's labelled row height");
	Require(label->GetPosition().y - field->GetPosition().y == baselineLabelOffset,
		"Clear button must preserve master's label alignment");
	auto *clear = dynamic_cast<wxBitmapButton *>(row->GetItem(size_t(1))->GetWindow());
	Require(clear && clear->IsShown(), "Clear button must always be visible");
	Require(clear->IsEnabled() == !entry->GetValue().IsEmpty(), "Initial enabled state");
	const wxSize buttonMin = clear->GetMinSize();
	// Fixed width, flexible height: the row stretches the button to the field's
	// height so the bitmap stays vertically centered (macOS bezel fix).
	const int side = std::max(field->GetBestSize().y, field->FromDIP(22));
	Require(buttonMin == wxSize(side, -1),
		"Clear button keeps a fixed width and stretches to the field's height");
	Require(clear->GetMaxSize() == wxSize(side, field->GetBestSize().y),
		"Clear button never raises the row above the field's height");
	Require((clear->GetWindowStyleFlag() & wxBORDER_MASK) != wxBORDER_NONE,
		"Clear button must look like a button");
	Require(row->GetItem(clear)->GetBorder() == 0,
		"Clear button must not add a gap before the X");
	Require(clear->GetPosition().x == field->GetPosition().x + field->GetSize().x,
		"Clear button must sit flush against the field");
	Require(row->GetMinSize().y == field->GetBestSize().y,
		"Clear button must not increase the row height");
	Require(clear->GetBitmap().GetLogicalSize() == clear->FromDIP(wxSize(16, 16)),
		"Clear icon must remain 16 DIP at every scale");
	CheckArtwork(clear->GetBitmap(), wxSYS_COLOUR_BTNTEXT);
	CheckArtwork(clear->GetBitmapDisabled(), wxSYS_COLOUR_GRAYTEXT);

	// Seed stale colours, then simulate a theme switch. Both states must be rebuilt.
	const auto stale =
		CamuleArtProvider::GetMenuBitmapBundle("menu_x_lg", wxSize(16, 16), wxColour(253, 1, 127));
	clear->SetBitmap(stale);
	clear->SetBitmapDisabled(stale);
	wxSysColourChangedEvent themeChanged;
	clear->GetEventHandler()->ProcessEvent(themeChanged);
	CheckArtwork(clear->GetBitmap(), wxSYS_COLOUR_BTNTEXT);
	CheckArtwork(clear->GetBitmapDisabled(), wxSYS_COLOUR_GRAYTEXT);
	Require(row->GetMinSize().y == field->GetBestSize().y,
		"Theme updates must preserve the field-height row");

	const wxRect geometry = field->GetRect();
	entry->SetValue("ubuntu");
	Require(clear->IsShown() && clear->IsEnabled(), "Text enables clear");
	Require(field->GetRect() == geometry, "Typing must not move the field");

	TextCounter counter;
	// Skip so the helper and existing application handlers receive the event.
	parent->Bind(wxEVT_TEXT, &TextCounter::OnText, &counter, id);
	wxCommandEvent click(wxEVT_BUTTON, clear->GetId());
	clear->GetEventHandler()->ProcessEvent(click);
	Require(entry->GetValue().IsEmpty(), "Button clears the field");
	Require(counter.count == 1, "Clear must propagate exactly one text event");
	Require(clear->IsShown() && !clear->IsEnabled(), "Empty clear stays visible and disabled");
	Require(field->GetRect() == geometry, "Clearing must not move the field");
	if (auto *combo = dynamic_cast<wxComboBox *>(field)) {
		Require(combo->GetCount() == 1 && combo->GetString(0) == "saved term",
			"Clearing text must preserve saved history");
	}

	// Search replaces its Name field in the same sizer when history is toggled.
	for (bool useCombo : { false, true }) {
		wxWindow *replacement = useCombo ? static_cast<wxWindow *>(new wxComboBox(parent, id))
						 : static_cast<wxWindow *>(new wxTextCtrl(parent, id));
		row->Replace(field, replacement);
		field->Destroy();
		field = replacement;
		entry = dynamic_cast<wxTextEntry *>(field);
		entry->SetValue("replacement");
		Require(clear->IsEnabled(), "Replacement text enables clear");
		clear->GetEventHandler()->ProcessEvent(click);
		Require(entry->GetValue().IsEmpty() && !clear->IsEnabled(), "Clear follows replacement ID");
		Require(clear->IsShown(), "Replacement empty state stays visible");
	}
	parent->Unbind(wxEVT_TEXT, &TextCounter::OnText, &counter, id);
}
} // namespace

int main(int argc, char **argv)
{
	InstallGuiTestDiagnostics();
	if (!wxEntryStart(argc, argv)) {
		std::cerr << "No GUI display: clear-button checks skipped\n";
		return 77;
	}
	RestoreGuiTestLog();
	if (!wxTheApp->CallOnInit()) {
		wxEntryCleanup();
		return 1;
	}
	wxInitAllImageHandlers();
	int status = 0;
	{
		wxGUIEventLoop loop;
		wxEventLoopActivator active(&loop);
		auto *frame = new wxFrame(nullptr, wxID_ANY, "Clear button regression checks");
		auto *outer = new wxBoxSizer(wxVERTICAL);
		frame->SetSizer(outer);
		frame->SetClientSize(600, 400);
		frame->Show();
		wxYield();
		try {
			for (double scale : { 1.0, 2.0 }) {
				CheckRow(new DpiTextCtrl(frame, scale), outer);
			}
			auto *combo = new wxComboBox(frame, wxID_ANY);
			combo->Append("saved term");
			combo->SetValue("initial text");
			CheckRow(combo, outer);
		} catch (const std::exception &error) {
			std::cerr << error.what() << '\n';
			status = 1;
		}
		frame->Destroy();
		wxYield();
	}
	wxTheApp->OnExit();
	wxEntryCleanup();
	return status;
}
