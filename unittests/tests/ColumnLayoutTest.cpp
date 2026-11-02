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

#include <wx/wx.h>
#include <wx/fileconf.h>
#include <wx/mstream.h>
#include <wx/menu.h>
#include "MuleVirtualDataViewCtrl.h"
#include "GuiTestDiagnostics.h"
#include "NativeListTest.h"
#include <common/MenuIDs.h>

namespace
{
class TestList : public CMuleVirtualDataViewCtrl
{
public:
	explicit TestList(wxWindow *parent)
	: CMuleVirtualDataViewCtrl(parent)
	{
		AddTextColumn("Part", 0, "P", 50);
		AddTextColumn("Name", 1, "N", 200);
		AddTextColumn("Size", 2, "S", 80);
		SetColumnHidden(0, true, 0);
		AppendSpacerColumn(3);
		AssociateVirtualModel();
		m_columnStore.SetTableName("LayoutTest");
		LoadColumnSettings();
		InitColumnState();
	}
	using CMuleDataViewCtrl::CopyColumnLayoutFrom;
	using CMuleDataViewCtrl::RestoreColumnLayout;
	int CachedWidth(int col) const { return m_columnStore.GetCachedWidth(col); }
	int DefaultWidth(int col) const { return m_columnStore.GetColumnDefaultWidth(col); }
	using CMuleDataViewCtrl::ApplySorting;
	using CMuleDataViewCtrl::SaveColumnSettings;
	wxPoint popupPosition = wxDefaultPosition;
	bool lastVisibleEnabled = true;
	TestList *mirror = nullptr;
	int visibilityChanges = 0;
	int widthChanges = 0;

protected:
	bool DoPopupMenu(wxMenu *menu, int x, int y) override
	{
		popupPosition = wxPoint(x, y);
		lastVisibleEnabled = menu->IsEnabled(MP_LISTCOL_1 + 1);
		return true;
	}
	wxString GetItemColumnText(wxUIntPtr, unsigned) const override { return "value"; }
	int CompareItemData(wxUIntPtr, wxUIntPtr, unsigned, bool, int) const override { return 0; }
	void OnColumnVisibilityChanged() override
	{
		++visibilityChanges;
		if (mirror) {
			mirror->CopyColumnLayoutFrom(*this);
		}
	}
	void OnColumnWidthsChanged() override
	{
		++widthChanges;
		CMuleDataViewCtrl::OnColumnWidthsChanged();
	}
};
class ConfigScope
{
public:
	ConfigScope()
	: input("", 0)
	, config(input)
	, previous(wxConfigBase::Set(&config))
	{
	}
	~ConfigScope() { wxConfigBase::Set(previous); }
	wxMemoryInputStream input;
	wxFileConfig config;
	wxConfigBase *previous;
};
} // namespace

void RunTests()
{
	ConfigScope fixture;
	{
		wxFrame frame(nullptr, wxID_ANY, "Column recovery test");
		auto *list = new TestList(&frame);
		auto *sizer = new wxBoxSizer(wxVERTICAL);
		sizer->Add(list, 1, wxEXPAND);
		frame.SetSizer(sizer);
		frame.SetSize(800, 500);
		frame.Show();
		wxTheApp->Yield(true);
		// GTK assigns native column widths when its first frame is painted.
		for (int attempt = 0; attempt < 100 && list->GetColumn(1)->GetWidth() == 0; ++attempt) {
			wxMilliSleep(10);
			wxTheApp->Yield(true);
		}
		Check(list->GetColumn(1)->GetWidth() > 0, "Native columns were not laid out");
		list->SetColumnHidden(2, true, 0);
		wxCommandEvent hide(wxEVT_MENU, MP_LISTCOL_1 + 1);
		list->ProcessWindowEvent(hide);
		Check(!list->IsColumnHidden(1), "Last visible column can be hidden");
		wxKeyEvent settings(wxEVT_KEY_DOWN);
		settings.m_keyCode = 'L';
		settings.SetShiftDown(true);
		// wx maps ControlDown()/CmdDown() to Command on macOS too.
		settings.SetControlDown(true);
		list->ProcessWindowEvent(settings);
		Check(list->popupPosition == wxPoint(0, 0), "Keyboard column menu did not open at the list");
		Check(!list->lastVisibleEnabled, "Menu allows hiding the last visible column");
		list->SetColumnHidden(2, false, 80);
		list->GetColumn(1)->SetWidth(333);
		wxTheApp->Yield(true);
		for (int attempt = 0; attempt < 100 && list->GetColumn(1)->GetWidth() != 333; ++attempt) {
			wxMilliSleep(10);
			wxTheApp->Yield(true);
		}
		const int adjustedWidth = list->GetColumn(1)->GetWidth();
		Check(adjustedWidth == 333, "Native resize did not take effect");
		wxFrame mirroredFrame(nullptr, wxID_ANY, "Live mirrored layout");
		auto *mirrored = new TestList(&mirroredFrame);
		list->mirror = mirrored;
		const int visibilityChanges = list->visibilityChanges;
		const int widthChanges = list->widthChanges;
		list->ApplySorting(2, CMuleDataViewCtrl::SORT_DES);
		list->SaveColumnSettings();
		const wxString sorting = fixture.config.Read("/eMule/TableOrderingLayoutTest");
		list->RestoreColumnLayout(false);
		Check(list->visibilityChanges == visibilityChanges + 1 && list->widthChanges == widthChanges,
			"Show-all must synchronize exactly once");
		wxTheApp->Yield(true);
		Check(!list->IsColumnHidden(0) && !list->IsColumnHidden(2),
			"Show-all did not reveal every column");
		Check(list->GetColumn(1)->GetWidth() == adjustedWidth, "Show-all changed an existing width");
		list->RestoreColumnLayout(true);
		Check(mirrored->CachedWidth(1) == list->DefaultWidth(1),
			"Reset mirrored a stale native width");
		wxTheApp->Yield(true);
		Check(list->IsColumnHidden(0), "Reset lost the constructor's hidden Part column");
		Check(!list->IsColumnHidden(1) && !list->IsColumnHidden(2),
			"Reset failed to restore default visibility");
		for (int attempt = 0; attempt < 100 && list->GetColumn(1)->GetWidth() == adjustedWidth;
			++attempt) {
			wxMilliSleep(10);
			wxTheApp->Yield(true);
		}
		if (list->GetColumn(1)->GetWidth() == adjustedWidth) {
			std::cerr << "Adjusted and reset widths both " << adjustedWidth << '\n';
		}
		Check(list->GetColumn(1)->GetWidth() != adjustedWidth,
			"Reset failed to restore default widths");
		Check(list->visibilityChanges == visibilityChanges + 2 && list->widthChanges == widthChanges,
			"Restore must synchronize each layout once through the visibility hook");
		Check(fixture.config.Read("/eMule/TableOrderingLayoutTest") == sorting,
			"Restoring the layout changed sorting");
		wxFrame reloadedFrame(nullptr, wxID_ANY, "Persisted reset");
		auto *reloaded = new TestList(&reloadedFrame);
		Check(reloaded->IsColumnHidden(0) && !reloaded->IsColumnHidden(1) &&
				!reloaded->IsColumnHidden(2),
			"Reset visibility was not persisted");
		Check(reloaded->CachedWidth(0) == list->DefaultWidth(0) &&
				reloaded->CachedWidth(1) == list->DefaultWidth(1),
			"Reset widths were not persisted");
		list->mirror = nullptr;
	}
	fixture.config.Write("/eMule/TableWidthsLayoutTest", "P:-444,N:300,S:80");
	{
		wxFrame first(nullptr, wxID_ANY, "Source layout");
		wxFrame second(nullptr, wxID_ANY, "Mirrored layout");
		auto *source = new TestList(&first);
		auto *mirror = new TestList(&second);
		Check(mirror->CachedWidth(0) == 444, "Hidden-width fixture did not load");
		mirror->CopyColumnLayoutFrom(*source);
		Check(mirror->CachedWidth(1) == 300,
			"Mirroring before first paint lost a restored visible width");
		wxCommandEvent toggleName(wxEVT_MENU, MP_LISTCOL_1 + 1);
		source->ProcessWindowEvent(toggleName);
		Check(source->IsColumnHidden(1) && source->CachedWidth(1) == 300,
			"Hiding before first paint lost a saved width");
		source->ProcessWindowEvent(toggleName);
		source->RestoreColumnLayout(false);
		Check(source->CachedWidth(1) == 300, "Show-all before first paint lost a saved width");
		Check(source->CachedWidth(0) == 444, "Show-all lost a hidden column's saved width");
		source->RestoreColumnLayout(true);
		mirror->CopyColumnLayoutFrom(*source);
		Check(mirror->IsColumnHidden(0), "Mirroring lost hidden state");
		Check(mirror->CachedWidth(0) == source->DefaultWidth(0),
			"Mirroring retained a hidden column's pre-reset width");
		// Copying is passive: it must not emit notifications which echo back to
		// the source and continuously relayout two search tabs.
		Check(mirror->visibilityChanges == 0 && mirror->widthChanges == 0,
			"Copying a layout emitted change notifications");
	}

	fixture.config.DeleteEntry("/eMule/TableWidthsScaleLayoutTest");
	fixture.config.Write("/eMule/TableWidthsLayoutTest", "P:-200,N:300,S:-80");
	{
		wxFrame frame(nullptr, wxID_ANY, "Runtime DPI change");
		auto *list = new TestList(&frame);
#ifdef wxHAS_DPI_INDEPENDENT_PIXELS
		// Retina/display DPI changes do not change logical column widths.
		constexpr int expectedPart = 200;
		constexpr int expectedName = 300;
		constexpr int expectedSize = 80;
#else
		constexpr int expectedPart = 300;
		constexpr int expectedName = 450;
		constexpr int expectedSize = 120;
#endif
		wxDPIChangedEvent larger(wxSize(96, 96), wxSize(144, 144));
		list->ProcessWindowEvent(larger);
		Check(list->CachedWidth(0) == expectedPart && list->CachedWidth(1) == expectedName &&
				list->CachedWidth(2) == expectedSize,
			"DPI change used the wrong coordinate units for cached widths");
		Check(list->IsColumnHidden(0) && list->IsColumnHidden(2),
			"DPI change altered column visibility");
		list->SaveColumnSettings();
		const wxString widths = fixture.config.Read("/eMule/TableWidthsLayoutTest");
		Check(widths.Contains(wxString::Format("P:-%d", expectedPart)) &&
				widths.Contains(wxString::Format("S:-%d", expectedSize)),
			"Saving after a DPI change persisted stale hidden widths");

		wxFrame reloadedFrame(nullptr, wxID_ANY, "Reload after DPI change");
		auto *reloaded = new TestList(&reloadedFrame);
		Check(reloaded->CachedWidth(0) == expectedPart && reloaded->CachedWidth(2) == expectedSize,
			"Restart after a DPI change lost hidden restore widths");
		wxDPIChangedEvent smaller(wxSize(144, 144), wxSize(96, 96));
		reloaded->ProcessWindowEvent(smaller);
		Check(reloaded->CachedWidth(0) == 200 && reloaded->CachedWidth(2) == 80,
			"Returning to the original DPI did not restore hidden cached widths");

		wxCommandEvent show(wxEVT_MENU, MP_LISTCOL_1);
		list->ProcessWindowEvent(show);
		Check(!list->IsColumnHidden(0) && list->CachedWidth(0) == expectedPart,
			"Showing a column after a DPI change lost its restore width");
		list->SaveColumnSettings();
		Check(fixture.config.Read("/eMule/TableWidthsLayoutTest")
				.Contains(wxString::Format("P:%d", expectedPart)),
			"Showing a column after a DPI change restored a stale width");
	}

	fixture.config.Write("/eMule/TableWidthsLayoutTest", "P:-50,N:-200,S:-80");
	{
		wxFrame frame(nullptr, wxID_ANY, "Hidden profile recovery test");
		auto *list = new TestList(&frame);
		Check(list->IsColumnHidden(0) && !list->IsColumnHidden(1),
			"All-hidden profile did not recover the default identifying column");
	}
}
