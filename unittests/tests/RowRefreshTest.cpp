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
#include "Preferences.h"

namespace
{
class TestList : public CMuleVirtualDataViewCtrl
{
public:
	explicit TestList(wxWindow *parent)
	: CMuleVirtualDataViewCtrl(parent)
	{
		for (size_t i = 0; i < values.size(); ++i) {
			values[i] = static_cast<int>(i) * 10;
		}
		AddTextColumn("Value", 0, "V", 200);
		AssociateVirtualModel();
		ApplySorting(0, 0);
		InitColumnState();
	}
	std::vector<int> values = std::vector<int>(513);
	bool live = false;
	using CMuleDataViewCtrl::ApplySorting;
	unsigned refreshes = 0;
	void Refresh(bool erase = true, const wxRect *rect = nullptr) override
	{
		++refreshes;
		CMuleVirtualDataViewCtrl::Refresh(erase, rect);
	}

protected:
	bool IsLiveSortColumn() const override { return live; }
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
struct TestPreferences : CPreferences
{
	static void SetLiveSort(bool enabled) { s_liveListSort = enabled; }
};
struct LiveSortScope
{
	LiveSortScope()
	: previous(CPreferences::LiveListSort())
	{
		TestPreferences::SetLiveSort(true);
	}
	~LiveSortScope() { TestPreferences::SetLiveSort(previous); }
	bool previous;
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
	ShowAndWaitForPaint(frame);
	wxTheApp->ProcessPendingEvents();
	long initialFirst = 0, initialLast = 0;
	for (int attempt = 0; attempt < 100; ++attempt) {
		if (list->GetVisibleRowRange(initialFirst, initialLast) && initialLast >= 3) {
			break;
		}
		wxMilliSleep(10);
		wxTheApp->ProcessPendingEvents();
		wxTheApp->Yield(true);
	}
	Check(list->GetVisibleRowRange(initialFirst, initialLast) && initialLast >= 3,
		"Native viewport was not laid out");
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
	ShowAndWaitForPaint(frame);
	wxTheApp->ProcessPendingEvents();
	wxVariant value;
	model->GetValueByRow(value, 1, 0);
	Check(value.GetString() == "15", "Restored view has stale row data");
	list->RemoveItemData(2);
	list->SortList();
	Check(model->GetCount() == 3, "Removal followed by sorting lost rows");
	list->ClearItemData();
	list->SortList();
	Check(model->GetCount() == 0, "Empty sort failed");

	// Exercise the shared production batch API used by both download and shared lists.
	list->BeginBatchUpdate(false);
	Check(!list->IsFrozen(), "Routine poll froze painting");
	Check(list->GetColumn(0)->IsSortKey(), "Empty poll removed the header sort key");
	list->EndBatchUpdate(false);
	Check(list->GetColumn(0)->IsSortKey(), "Empty poll lost the sort key");

	list->BeginBatchUpdate(false);
	list->AppendItemDataNow(3);
	list->AppendItemDataNow(1);
	Check(!list->GetColumn(0)->IsSortKey(), "Insertion did not suspend native per-row sorting");
	Check(model->GetCount() == 2, "Batched insertion did not publish row count");
	list->EndBatchUpdate(false);
	Check(list->ItemAt(0) == 1 && list->ItemAt(1) == 3,
		"Newly visible rows were left unsorted when caller reported no new files");
	Check(list->GetColumn(0)->IsSortKey() && !list->IsFrozen(),
		"Batch did not restore header and painting");

	observer->changed.clear();
	list->refreshes = 0;
	list->BeginBatchUpdate(false);
	list->values[1] = 11;
	list->RefreshItemData(1);
	list->EndBatchUpdate(false);
	Check(observer->changed == std::vector<unsigned>({ 0 }) && list->refreshes == 0,
		"In-place poll did not retain a single row notification");

	// Reconnect and startup retain freezing, with startup allowed to thaw early.
	list->Freeze();
	list->BeginBatchUpdate(false);
	list->EndBatchUpdate(false);
	Check(list->IsFrozen(), "Nonfreezing batch thawed its caller's freeze");
	list->Thaw();
	list->BeginBatchUpdate();
	Check(list->IsFrozen(), "Reconnect batch did not freeze painting");
	list->RemoveItemData(3);
	list->AppendItemDataNow(2);
	list->EndBatchUpdate();
	Check(!list->IsFrozen() && list->ItemAt(0) == 1 && list->ItemAt(1) == 2,
		"Reconnect add/prune did not restore a sorted view");
	list->ClearItemData();
	list->BeginBatchUpdate();
	list->AppendItemData(4);
	list->AppendItemData(1);
	Check(list->FlushBatchRows(), "Startup did not flush pending rows");
	Check(!list->FlushBatchRows(), "Unchanged startup tick flushed again");
	list->ThawForDisplay();
	list->ThawForDisplay();
	Check(!list->IsFrozen(), "Startup early thaw did not release painting");
	list->AppendItemData(3);
	// Final drain completion must publish even without a preceding timer tick.
	list->EndBatchUpdate(false);
	Check(!list->IsFrozen() && model->GetCount() == 3 && list->ItemAt(1) == 3,
		"Startup drain failed after early thaw");

	// Exercise the scheduled path, rather than only calling SortList directly.
	{
		LiveSortScope preference;
		list->live = true;
		list->values[4] = 5;
		list->RefreshItemData(4);
		list->RefreshItemData(4);
		wxTheApp->ProcessPendingEvents();
		wxTheApp->Yield(true);
		Check(list->ItemAt(0) == 4, "Scheduled live sort did not run");
		list->values[4] = 50;
		list->RefreshItemData(4);
		list->RemoveItemData(4);
		wxTheApp->ProcessPendingEvents();
		wxTheApp->Yield(true);
		Check(!list->HasItemData(4) && model->GetCount() == 2,
			"Pending live sort resurrected a removed row");
		frame.Hide();
		list->values[3] = 1;
		list->RefreshItemData(3);
		wxTheApp->ProcessPendingEvents();
		wxTheApp->Yield(true);
		Check(list->ItemAt(0) == 1, "Hidden live sort moved rows");
		ShowAndWaitForPaint(frame);
		list->RefreshItemData(3);
		wxTheApp->ProcessPendingEvents();
		wxTheApp->Yield(true);
		Check(list->ItemAt(0) == 3, "Live sort did not recover after showing the view");
		list->live = false;
	}

	// A live refresh during startup must not consume unpublished appended rows.
	{
		LiveSortScope preference;
		list->live = true;
		list->BeginBatchUpdate();
		list->AppendItemData(4);
		const unsigned publishedCount = model->GetCount();
		list->values[1] = 0;
		list->RefreshItemData(1);
		wxTheApp->ProcessPendingEvents();
		wxTheApp->Yield(true);
		Check(model->GetCount() == publishedCount && list->ItemAt(0) == 3,
			"Queued live sort ran inside an unfinished startup batch");
		list->FinishBulkLoad();
		list->EndBatchUpdate(false);
		wxTheApp->ProcessPendingEvents();
		wxTheApp->Yield(true);
		Check(model->GetCount() == 3 && list->ItemAt(0) == 1,
			"Startup batch lost rows or its pending live update");
		list->live = false;
	}

	// A preference changed while a batch deferred the callback still wins.
	{
		LiveSortScope preference;
		list->live = true;
		list->BeginBatchUpdate(false);
		list->values[4] = -1;
		list->RefreshItemData(4);
		wxTheApp->ProcessPendingEvents();
		TestPreferences::SetLiveSort(false);
		list->EndBatchUpdate(false);
		wxTheApp->ProcessPendingEvents();
		Check(list->ItemAt(0) == 1, "Deferred sort ignored the disabled live-sort preference");
		list->live = false;
	}

	// A small visible window into a larger list must keep its scroll position.
	list->ClearItemData();
	for (wxUIntPtr data = 1; data <= 200; ++data) {
		list->values[data] = static_cast<int>(data) * 10;
		list->AppendItemData(data);
	}
	list->FinishBulkLoad();
	long first = 0, last = 0;
	for (int attempt = 0; attempt < 100; ++attempt) {
		list->EnsureVisible(model->GetItem(100));
		wxTheApp->ProcessPendingEvents();
		wxTheApp->Yield(true);
		if (list->GetVisibleRowRange(first, last) && first > 0) {
			break;
		}
		wxMilliSleep(10);
	}
	Check(list->GetVisibleRowRange(first, last) && first > 0, "Scroll fixture did not leave the top");
	const auto a = list->ItemAt(first + 1), b = list->ItemAt(first + 2);
	list->SetSelectedItemData({ a });
	list->SetCurrentItem(model->GetItem(first + 1));
	std::swap(list->values[a], list->values[b]);
	observer->changed.clear();
	list->SortList();
	long after = 0, afterLast = 0;
	Check(list->GetVisibleRowRange(after, afterLast) && after == first, "Sort moved the viewport");
	Check(observer->changed == std::vector<unsigned>({ static_cast<unsigned>(first + 1),
					   static_cast<unsigned>(first + 2) }),
		"Scrolled sort did not notify exactly the moved visible rows");
	Check(list->GetSelectedItemData() == std::vector<wxUIntPtr>({ a }) &&
			list->ItemAt(model->GetRow(list->GetCurrentItem())) == a,
		"Scrolled sort lost selection or cursor");
	observer->changed.clear();
	std::swap(list->values[199], list->values[200]);
	list->SortList();
	Check(observer->changed.empty(), "Off-screen reorder notified invisible rows");
	list->EnsureVisible(model->GetItem(199));
	wxTheApp->ProcessPendingEvents();
	wxTheApp->Yield(true);
	Check(list->ItemAt(199) == 199, "Scrolling revealed stale order");

	// A sort during a startup drain, such as a header click, must not notify unpublished rows.
	list->ClearItemData();
	list->BeginBatchUpdate(false);
	list->AppendItemData(3);
	list->AppendItemData(1);
	list->AppendItemData(2);
	observer->changed.clear();
	list->SortList();
	Check(observer->changed.empty(), "Sort notified rows the model does not have");
	list->EndBatchUpdate(false);
	Check(model->GetCount() == 3 && list->ItemAt(0) == 1, "Drain rows were not published");

	// Destruction must discard a queued live-sort callback.
	{
		LiveSortScope preference;
		auto *temporary = new TestList(&frame);
		temporary->live = true;
		temporary->AddItemData(1);
		temporary->RefreshItemData(1);
		delete temporary;
		wxTheApp->ProcessPendingEvents();
		wxTheApp->Yield(true);
	}
}
