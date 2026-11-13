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
#include <algorithm>
#include <memory>
#include <vector>
#include <wx/dcmemory.h>
#include "SearchColumnFit.h"
#include "ClientNameCell.h"
#include "Constants.h"
#include <stdexcept>
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
		AddTextColumn("Name", 0, "N", 100);
		AddBarColumn("Composite name", 1, "C", 80);
		AddBarColumn("Progress", 2, "P", 120);
		AppendSpacerColumn(3);
		AssociateVirtualModel();
		ApplySorting(0, 0);
		InitColumnState();
		EnableCellToolTips();
		SetSize(1000, 400);
	}
	using CMuleDataViewCtrl::FitColumnWidths;
	using CMuleDataViewCtrl::RecheckCellToolTip;
	using CMuleVirtualDataViewCtrl::GetColumnFitRows;
	mutable unsigned measurements = 0;

	//! Moves the pointer to `point` in the control's client coordinates.
	void MoveTo(const wxPoint &point)
	{
		wxWindow *body = GetMainWindow();
		wxMouseEvent motion(wxEVT_MOTION);
		motion.SetEventObject(body);
		motion.SetPosition(body->ScreenToClient(ClientToScreen(point)));
		body->GetEventHandler()->ProcessEvent(motion);
	}

protected:
	void GetDisplayOrder(wxDataViewItemArray &) const override
	{
		throw std::runtime_error("Fitter materialized the full display order");
	}
	wxString GetItemColumnText(wxUIntPtr data, unsigned) const override
	{
		return data == 4096 ? wxString('W', 25) : wxString("short");
	}
	int GetCustomFitWidth(const wxDataViewItem &, unsigned column, wxDC &) const override
	{
		++measurements;
		return column == 1 ? 350 : -1;
	}
	int CompareItemData(wxUIntPtr a, wxUIntPtr b, unsigned, bool, int) const override
	{
		return (a > b) - (a < b);
	}
	wxString GetCellToolTip(const wxDataViewItem &item, unsigned column) const override
	{
		return column == 1 ? wxString::Format("tip %u", unsigned(ItemAt(GetModelRow(item))))
				   : wxString();
	}
};
// The native model exposes only shown rows, just like CSearchListModel and
// CBrowseListModel. The production search/browse sampler must use that view
// instead of re-sampling unfiltered result storage.
struct SearchNode
{
	SearchNode *parent = nullptr;
	bool shown = true;
	std::vector<std::unique_ptr<SearchNode>> children;
};

class SearchModel : public wxDataViewModel
{
public:
	std::vector<std::unique_ptr<SearchNode>> roots;
	mutable size_t childQueries = 0;
	unsigned GetColumnCount() const override { return 1; }
	wxString GetColumnType(unsigned) const override { return "string"; }
	void GetValue(wxVariant &value, const wxDataViewItem &, unsigned) const override { value = "row"; }
	bool SetValue(const wxVariant &, const wxDataViewItem &, unsigned) override { return false; }
	wxDataViewItem GetParent(const wxDataViewItem &item) const override
	{
		return item.IsOk() ? wxDataViewItem(static_cast<SearchNode *>(item.GetID())->parent)
				   : wxDataViewItem();
	}
	bool IsContainer(const wxDataViewItem &item) const override
	{
		return !item.IsOk() || !static_cast<SearchNode *>(item.GetID())->children.empty();
	}
	unsigned GetChildren(const wxDataViewItem &item, wxDataViewItemArray &children) const override
	{
		++childQueries;
		const auto &nodes = item.IsOk() ? static_cast<SearchNode *>(item.GetID())->children : roots;
		for (const auto &node : nodes) {
			if (node->shown) {
				children.Add(wxDataViewItem(node.get()));
			}
		}
		return children.size();
	}
	SearchNode *Add(SearchNode *parent = nullptr)
	{
		auto node = std::make_unique<SearchNode>();
		node->parent = parent;
		SearchNode *result = node.get();
		(parent ? parent->children : roots).push_back(std::move(node));
		return result;
	}
};

void TestSearchSampling(wxWindow *parent)
{
	wxDataViewCtrl view(parent, wxID_ANY);
	view.AppendTextColumn("Search result", 0);
	auto *model = new SearchModel;
	view.AssociateModel(model);
	model->DecRef();
	wxDataViewItemArray rows;
	const auto has = [&](SearchNode *node) {
		return std::find(rows.begin(), rows.end(), wxDataViewItem(node)) != rows.end();
	};
	for (const size_t count : { 513u, 2000u, 4096u, 20000u }) {
		model->roots.clear();
		for (size_t i = 0; i < count; ++i) {
			model->Add();
		}
		model->Cleared();
		model->childQueries = 0;
		SampleSearchColumnFitRows(view, rows, 512);
		Check(rows.size() == 512 && has(model->roots.front().get()) && has(model->roots.back().get()),
			"Search sample omitted the first or last root");
		Check(model->childQueries == 1, "Flat search sample walked descendant lists");
	}
	// Four matches deliberately away from either end of the old raw-storage stride.
	for (auto &root : model->roots) {
		root->shown = false;
	}
	for (const size_t index : { 1001u, 7003u, 13007u, 19997u }) {
		model->roots[index]->shown = true;
	}
	model->Cleared();
	SampleSearchColumnFitRows(view, rows, 512);
	Check(rows.size() == 4 && has(model->roots[1001].get()) && has(model->roots[19997].get()),
		"Sparse filtering lost visible search results");

	model->roots.clear();
	SearchNode *folder = model->Add();
	SearchNode *lastFolder = model->Add();
	for (size_t i = 0; i < 2000; ++i) {
		model->Add(folder);
	}
	model->Cleared();
	SampleSearchColumnFitRows(view, rows, 512);
	Check(rows.size() == 2 && has(folder) && has(lastFolder),
		"Collapsed browse folders were not sampled");
	view.Expand(wxDataViewItem(folder));
	SampleSearchColumnFitRows(view, rows, 512);
	Check(rows.size() == 512 && has(folder) && has(lastFolder) && has(folder->children.front().get()) &&
			has(folder->children.back().get()),
		"Expanded browse sample missed roots or late children");
	// Matching variant after the first eight children; its container remains visible.
	for (auto &child : folder->children) {
		child->shown = false;
	}
	folder->children[1573]->shown = true;
	model->Cleared();
	view.Expand(wxDataViewItem(folder));
	SampleSearchColumnFitRows(view, rows, 512);
	Check(rows.size() == 3 && has(folder->children[1573].get()),
		"Matching grouped child was not sampled");
	view.SetCurrentItem(wxDataViewItem(folder->children[1573].get()));
	SampleSearchColumnFitRows(view, rows, 512);
	Check(rows.size() == 3, "Current row was sampled twice");
	view.Collapse(wxDataViewItem(folder));
	SampleSearchColumnFitRows(view, rows, 512);
	Check(!has(folder->children[1573].get()), "Collapsed current child was measured");
	for (size_t limit : { 0u, 1u, 2u }) {
		SampleSearchColumnFitRows(view, rows, limit);
		Check(rows.size() <= limit, "Small sample exceeded its budget");
	}

	// Many expanded groups used to exhaust the budget before reaching late roots.
	model->roots.clear();
	for (size_t i = 0; i < 2000; ++i) {
		SearchNode *group = model->Add();
		for (size_t j = 0; j < 10; ++j) {
			model->Add(group);
		}
	}
	model->Cleared();
	view.Expand(wxDataViewItem(model->roots.front().get()));
	view.Expand(wxDataViewItem(model->roots.back().get()));
	SampleSearchColumnFitRows(view, rows, 512);
	Check(rows.size() <= 512 && has(model->roots.front().get()) && has(model->roots.back().get()) &&
			has(model->roots.front()->children.back().get()) &&
			has(model->roots.back()->children.back().get()),
		"Expanded groups consumed the root sample budget");
}

// Draws one cell and compares its text-coloured pixels with the expected layout.
void CheckClientCell(const ClientNameCell &cell, const CClientImages &images, const wxBitmap &flag, int scale)
{
	wxBitmap actual;
	actual.CreateWithDIPSize(wxSize(500, 40), scale);
	wxMemoryDC dc(actual);
	dc.SetBackground(*wxWHITE_BRUSH);
	dc.Clear();
	dc.SetFont(*wxNORMAL_FONT);
	dc.SetTextForeground(*wxRED);
	const wxString text = cell.name.empty() ? wxString("?") : cell.name;
	const int slot = dc.FromDIP(16) + dc.FromDIP(2);
	const int badgeX = cell.showState ? 2 * slot : slot;
	const wxSize badgeSize = dc.FromDIP(wxSize(10, 10));
	const int badgeStep = badgeSize.x + dc.FromDIP(1);
	const int textX = badgeX + 3 * badgeStep - dc.FromDIP(1) + dc.FromDIP(2) +
			  (flag.IsOk() ? flag.GetLogicalSize().x + dc.FromDIP(2) : 0);
	Check(MeasureClientNameCell(cell, dc, images, flag) == textX + dc.GetTextExtent(text).x,
		"Client measurement differs from resource sizes");
	DrawClientNameCell(cell, wxRect(0, 0, 500, 40), dc, images, flag);
	wxBitmap expected;
	expected.CreateWithDIPSize(wxSize(500, 40), scale);
	wxMemoryDC expectedDC(expected);
	expectedDC.SetBackground(*wxWHITE_BRUSH);
	expectedDC.Clear();
	expectedDC.SetFont(*wxNORMAL_FONT);
	expectedDC.SetTextForeground(*wxRED);
	expectedDC.DrawText(text, textX, (40 - expectedDC.GetCharHeight()) / 2 + 1);
	if (cell.obfuscation == OBST_ENABLED) {
		// The test icons are solid squares, so the lock is one too.
		const int iconY = (40 - dc.FromDIP(16)) / 2 + 1;
		expectedDC.SetPen(*wxTRANSPARENT_PEN);
		expectedDC.SetBrush(*wxRED_BRUSH);
		expectedDC.DrawRectangle(
			wxPoint(badgeX + badgeStep, iconY + (dc.FromDIP(16) - badgeSize.y) / 2), badgeSize);
	}
	dc.SelectObject(wxNullBitmap);
	expectedDC.SelectObject(wxNullBitmap);
	const wxImage drawn = actual.ConvertToImage();
	const wxImage reference = expected.ConvertToImage();
	// Compare only text-coloured pixels: the name, which icons, flags and empty badge
	// positions must not shift, and the lock, which takes the text colour.
	for (int y = 0; y < drawn.GetHeight(); ++y) {
		for (int x = 0; x < drawn.GetWidth(); ++x) {
			const auto red = [&](const wxImage &image) {
				return image.GetRed(x, y) > image.GetGreen(x, y) &&
				       image.GetGreen(x, y) == image.GetBlue(x, y);
			};
			Check(red(drawn) == red(reference),
				"Drawn client text disagrees with measured layout");
		}
	}
}

void TestClientToolTip()
{
	ClientNameCell cell;
	Check(ClientNameCellToolTip(cell).empty(), "A cell without badges has a tooltip");
	cell.identFailed = true;
	cell.badGuy = true;
	Check(ClientNameCellToolTip(cell) == "Secure ident: Failed",
		"Failed secure ident not named as in Client Details");
	cell.identFailed = false;
	Check(ClientNameCellToolTip(cell) == "Secure ident: Bad Guy",
		"Bad guy not named as in Client Details");
	cell.badGuy = false;
	cell.identified = true;
	cell.obfuscation = OBST_ENABLED;
	cell.highCredits = true;
	cell.extProtocol = false;
	Check(ClientNameCellToolTip(cell) == "Secure ident: Verified - OK\nObfuscation: Enabled\n"
					     "Credits: Moves up faster in our upload queue\n"
					     "Does not support the eMule extended protocol",
		"Tooltip does not list every badge");
}

void TestClientDrawing()
{
	for (const int scale : { 1, 2 }) {
		CClientImages images;
		wxBitmap icon(16, 16);
		{
			wxMemoryDC iconDC(icon);
			iconDC.SetBackground(*wxGREEN_BRUSH);
			iconDC.Clear();
		}
		for (unsigned i = 0; i < 64; ++i) {
			images.Add(wxBitmapBundle(icon));
		}
		wxBitmap flag;
		flag.CreateWithDIPSize(wxSize(16, 12), scale);
		{
			wxMemoryDC flagDC(flag);
			flagDC.SetBackground(*wxCYAN_BRUSH);
			flagDC.Clear();
		}
		for (const bool state : { false, true }) {
			for (const bool withFlag : { false, true }) {
				for (const bool badges : { false, true }) {
					for (const wxString &name : { wxString(), wxString("Wide name") }) {
						ClientNameCell cell;
						cell.name = name;
						cell.showState = state;
						cell.highCredits = badges;
						cell.identified = badges;
						cell.obfuscation = badges ? OBST_ENABLED : OBST_UNDEFINED;
						CheckClientCell(
							cell, images, withFlag ? flag : wxBitmap(), scale);
					}
				}
			}
		}
	}
}

} // namespace

void RunTests()
{
	wxFrame frame(nullptr, wxID_ANY, "Column fitting test");
	TestSearchSampling(&frame);
	TestClientToolTip();
	TestClientDrawing();
	auto *list = new TestList(&frame);
	for (wxUIntPtr i = 1; i <= 4096; ++i) {
		list->AppendItemData(i);
	}
	list->FinishBulkLoad();
	auto *sizer = new wxBoxSizer(wxVERTICAL);
	sizer->Add(list, 1, wxEXPAND);
	frame.SetSizer(sizer);
	frame.SetSize(1000, 500);
	ShowAndWaitForPaint(frame);
	const wxDataViewItem first = static_cast<wxDataViewVirtualListModel *>(list->GetModel())->GetItem(0);
	// GTK sizes the columns just after the map.
	const auto laidOut = [&] { return list->GetItemRect(first, list->GetColumn(1)).width > 0; };
	for (int attempt = 0; attempt < 300 && !laidOut(); ++attempt) {
		wxMilliSleep(10);
		wxTheApp->Yield(true);
	}
	Check(laidOut(), "List was not laid out");
	wxWindow *body = list->GetMainWindow();
	list->MoveTo(list->GetItemRect(first, list->GetColumn(1)).GetPosition() + wxPoint(5, 2));
	Check(body->GetToolTipText() == "tip 1", "Cell tooltip not shown");
	// Rows change under a still pointer. A row moving elsewhere keeps the shown tooltip as it
	// is; a row moving away replaces it.
	const wxToolTip *shown = body->GetToolTip();
	list->RemoveItemData(2);
	list->RecheckCellToolTip();
	Check(body->GetToolTip() == shown, "Cell tooltip replaced while its row stayed");
	list->RemoveItemData(1);
	list->RecheckCellToolTip();
	Check(body->GetToolTipText() == "tip 3", "Cell tooltip kept a row that moved away");
	list->AddItemData(1);
	list->AddItemData(2);
	list->MoveTo(list->GetItemRect(first, list->GetColumn(0)).GetPosition() + wxPoint(5, 2));
	Check(body->GetToolTip() == nullptr, "Cell tooltip outlived its cell");
	list->RecheckCellToolTip();
	Check(body->GetToolTip() == nullptr, "Recheck added a tooltip the pointer never asked for");
	wxDataViewItemArray sample;
	list->GetColumnFitRows(sample, 512);
	Check(sample.size() == 512, "Fit sample is not bounded");
	Check(list->ItemAt(list->GetModelRow(sample.front())) == 1 &&
			list->ItemAt(list->GetModelRow(sample.back())) == 4096,
		"Fit sample omitted either end of the list");
	list->FitColumnWidths(-1);
	wxTheApp->Yield(true);
	for (int attempt = 0; attempt < 100 && list->GetColumn(1)->GetWidth() < 350; ++attempt) {
		wxMilliSleep(10);
		wxTheApp->Yield(true);
	}
	Check(list->GetColumn(0)->GetWidth() > 100, "Long sampled text was not fitted");
	Check(list->GetColumn(1)->GetWidth() >= 350,
		"Composite cell used a fixed bar width instead of measured content");
	const int barWidth = list->GetColumn(2)->GetWidth();
	const int nameWidth = list->GetColumn(1)->GetWidth();
	list->FitColumnWidths(-1);
	wxTheApp->Yield(true);
	Check(list->GetColumn(2)->GetWidth() == barWidth && list->GetColumn(1)->GetWidth() == nameWidth,
		"Repeated fit grows widths using their previous values");
	list->SetColumnHidden(1, true, 0);
	list->FitColumnWidths(-1);
	wxTheApp->Yield(true);
	Check(list->IsColumnHidden(1), "Fitting revealed a hidden column");
}
