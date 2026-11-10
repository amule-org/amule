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
#include "MuleVirtualDataViewCtrl.h"
#include "GuiTestDiagnostics.h"
#include "NativeListTest.h"

namespace
{
class TestList : public CMuleVirtualDataViewCtrl
{
public:
	explicit TestList(wxWindow *parent)
	: CMuleVirtualDataViewCtrl(parent)
	{
		AddTextColumn("Value", 0, "V", 200);
		AssociateVirtualModel();
		ApplySorting(0, 0);
		InitColumnState();
	}
	int values[5] = { 0, 10, 20, 30, 40 };
	unsigned refreshes = 0;
	void Refresh(bool erase = true, const wxRect *rect = nullptr) override
	{
		++refreshes;
		CMuleVirtualDataViewCtrl::Refresh(erase, rect);
	}

protected:
	wxString GetItemColumnText(wxUIntPtr data, unsigned) const override
	{
		return wxString::Format("%d", values[data]);
	}
	int CompareItemData(wxUIntPtr a, wxUIntPtr b, unsigned, bool, int modifier) const override
	{
		return ((values[a] > values[b]) - (values[a] < values[b])) * modifier;
	}
};

class Observer : public wxDataViewModelNotifier
{
public:
	std::vector<unsigned> changed;
	unsigned resets = 0;
	bool ItemChanged(const wxDataViewItem &item) override
	{
		changed.push_back(static_cast<wxDataViewListModel *>(GetOwner())->GetRow(item));
		return true;
	}
	bool ItemAdded(const wxDataViewItem &, const wxDataViewItem &) override { return true; }
	bool ItemDeleted(const wxDataViewItem &, const wxDataViewItem &) override { return true; }
	bool ValueChanged(const wxDataViewItem &, unsigned) override { return true; }
	bool Cleared() override
	{
		++resets;
		return true;
	}
	void Resort() override {}
};
struct ConfigScope
{
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
	ConfigScope config;
	wxFrame frame(nullptr, wxID_ANY, "Row refresh regression");
	auto *list = new TestList(&frame);
	auto *sizer = new wxBoxSizer(wxVERTICAL);
	sizer->Add(list, 1, wxEXPAND);
	frame.SetSizer(sizer);
	frame.SetSize(600, 400);
	for (wxUIntPtr data = 1; data <= 4; ++data)
		list->AppendItemData(data);
	list->FinishBulkLoad();
	frame.Show();
	wxTheApp->Yield(true);
	Check(list->ItemAt(0) == 1 && list->ItemAt(3) == 4, "Fixture is not ascending");
	auto *observer = new Observer;
	list->GetModel()->AddNotifier(observer); // Owned by the model.
	auto *model = static_cast<wxDataViewVirtualListModel *>(list->GetModel());
	list->SetSelectedItemData({ 2 });
	list->SetCurrentItem(model->GetItem(1));
	list->refreshes = 0;
	list->SortList();
	Check(list->refreshes == 0, "Unchanged sort repainted the whole list");
	Check(observer->changed.empty() && observer->resets == 0, "Unchanged sort notified rows");

	list->values[2] = 35;
	list->SortList();
	Check(list->ItemAt(0) == 1 && list->ItemAt(1) == 3 && list->ItemAt(2) == 2 && list->ItemAt(3) == 4,
		"Changed value did not reorder rows");
	Check(observer->changed == std::vector<unsigned>({ 1, 2 }),
		"Sort did not notify exactly the moved rows");
	Check(list->refreshes == 0 && observer->resets == 0, "Sort repainted or reset the whole list");
	Check(list->GetSelectedItemData() == std::vector<wxUIntPtr>({ 2 }), "Sort changed selected identity");
	Check(list->ItemAt(model->GetRow(list->GetCurrentItem())) == 2, "Sort changed cursor identity");
	observer->changed.clear();
	list->SortList();
	Check(observer->changed.empty() && list->refreshes == 0, "Repeated sort repainted unchanged rows");

	// A data update that does not move the row must still notify that row.
	list->values[2] = 36;
	list->RefreshItemData(2);
	Check(observer->changed == std::vector<unsigned>({ 2 }), "Data update missed its row");
	observer->changed.clear();
	list->SortList();
	Check(observer->changed.empty(), "Stable data update caused extra sort notifications");

	frame.Hide();
	list->values[2] = 15;
	list->SortList();
	Check(observer->changed.empty(), "Hidden sort notified invisible rows");
	frame.Show();
	wxTheApp->Yield(true);
	wxVariant value;
	model->GetValueByRow(value, 1, 0);
	Check(value.GetString() == "15", "Restored view has stale row data");
	list->RemoveItemData(2);
	list->SortList();
	Check(model->GetCount() == 3, "Removal followed by sorting lost rows");
	list->ClearItemData();
	list->SortList();
	Check(model->GetCount() == 0, "Empty sort failed");
}
