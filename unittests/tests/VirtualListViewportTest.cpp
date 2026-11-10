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
#include "NativeListTest.h"
#include "MuleVirtualDataViewCtrl.h"
#include "Preferences.h"

#include <wx/wx.h>
#include <wx/fileconf.h>
#include <wx/mstream.h>

namespace
{
class TestPreferences : public CPreferences
{
public:
	static void SetLiveSort(bool enabled) { s_liveListSort = enabled; }
};

// Observe model notifications independently of the native backend's repaint behavior.
class ChangeObserver : public wxDataViewModelNotifier
{
public:
	unsigned changes = 0;
	bool ItemAdded(const wxDataViewItem &, const wxDataViewItem &) override { return true; }
	bool ItemDeleted(const wxDataViewItem &, const wxDataViewItem &) override { return true; }
	bool ItemChanged(const wxDataViewItem &) override
	{
		++changes;
		return true;
	}
	bool ValueChanged(const wxDataViewItem &, unsigned) override { return true; }
	bool Cleared() override { return true; }
	void Resort() override {}
};

class TestList : public CMuleVirtualDataViewCtrl
{
public:
	TestList(wxWindow *parent, bool genericEmptyViewport, bool reverse)
	: CMuleVirtualDataViewCtrl(parent)
	, m_genericEmptyViewport(genericEmptyViewport)
	, m_reverse(reverse)
	{
		AddTextColumn("Download", 0, "N", 250);
		AssociateVirtualModel();
		ApplySorting(0, 0);
		InitColumnState();
	}
	wxDataViewItem GetTopItem() const override
	{
		if (unknownViewport) {
			return wxDataViewItem();
		}
		// wx's generic backend (Windows) reports row zero even for an empty virtual model.
		if (m_genericEmptyViewport && PublishedCount() == 0) {
			return Model()->GetItem(0);
		}
		return CMuleVirtualDataViewCtrl::GetTopItem();
	}
	int GetCountPerPage() const override
	{
		if (m_genericEmptyViewport && PublishedCount() == 0) {
			return 10;
		}
		return CMuleVirtualDataViewCtrl::GetCountPerPage();
	}
	bool unknownViewport = false;
	bool liveSort = false;
	mutable unsigned comparisons = 0;
	bool IsLiveSortColumn() const override { return liveSort; }
	unsigned PublishedCount() const { return Model()->GetCount(); }
	void ShowRow(unsigned row) { EnsureVisible(Model()->GetItem(row)); }
	long TopRow() const
	{
		long first = -1, last = -1;
		Check(GetVisibleRowRange(first, last), "Populated list has no visible range");
		return first;
	}
	using CMuleVirtualDataViewCtrl::ItemAt;

private:
	const wxDataViewVirtualListModel *Model() const
	{
		return static_cast<const wxDataViewVirtualListModel *>(GetModel());
	}
	wxString GetItemColumnText(wxUIntPtr data, unsigned) const override
	{
		return wxString::Format("Download %03lu", static_cast<unsigned long>(data));
	}
	int CompareItemData(wxUIntPtr a, wxUIntPtr b, unsigned, bool, int) const override
	{
		++comparisons;
		return ((a > b) - (a < b)) * (m_reverse ? -1 : 1);
	}
	bool m_genericEmptyViewport;
	bool m_reverse;
};

void Settle()
{
	for (int i = 0; i < 10; ++i) {
		wxTheApp->Yield(true);
		wxMilliSleep(10);
	}
}

void TestBulkLoad(bool genericEmptyViewport, bool reverse)
{
	wxFrame frame(nullptr, wxID_ANY, "Download startup scroll test", wxDefaultPosition, wxSize(600, 350));
	auto *list = new TestList(&frame, genericEmptyViewport, reverse);
	frame.Show();
	Settle();
	list->Freeze();
	list->ClearItemData();
	for (unsigned i = 1; i <= 200; ++i) {
		list->AppendItemData(i);
	}
	Check(list->PublishedCount() == 0, "Deferred rows were published before bulk completion");
	list->FinishBulkLoad();
	list->Thaw();
	Settle();
	Check(list->PublishedCount() == 200, "Bulk completion lost download rows");
	Check(list->TopRow() == 0, "Initial bulk load scrolled away from the top");
	Check(list->ItemAt(0) == (reverse ? 200 : 1), "Initial sort order is wrong");

	// A real, already displayed scroll position must survive a clear/refill.
	list->ShowRow(100);
	Settle();
	Check(list->TopRow() > 0, "Fixture did not scroll the populated list");
	wxUIntPtr anchor = list->ItemAt(list->TopRow());
	list->Freeze();
	list->ClearItemData();
	for (unsigned i = 1; i <= 200; ++i) {
		list->AppendItemData(i);
	}
	list->FinishBulkLoad();
	list->Thaw();
	Settle();
	// GTK may leave a fraction of the preceding row visible after EnsureVisible.
	Check(list->ItemAt(list->TopRow()) == anchor || list->ItemAt(list->TopRow() + 1) == anchor,
		"Rebuild discarded the existing scroll anchor");

	// Appending to a published list keeps its current anchor as well.
	anchor = list->ItemAt(list->TopRow());
	list->Freeze();
	list->AppendItemData(201);
	list->FinishBulkLoad();
	list->Thaw();
	Settle();
	Check(list->PublishedCount() == 201, "Append did not publish the new row");
	Check(list->ItemAt(list->TopRow()) == anchor || list->ItemAt(list->TopRow() + 1) == anchor,
		"Append discarded the existing scroll anchor");

	// Clearing to empty and completing that load must not resurrect an old anchor later.
	list->ClearItemData();
	list->FinishBulkLoad();
	Settle();
	long first = -1, last = -1;
	Check(!list->GetVisibleRowRange(first, last), "Empty model reports a visible download");
	for (unsigned i = 1; i <= 200; ++i) {
		list->AppendItemData(i);
	}
	list->FinishBulkLoad();
	Settle();
	Check(list->TopRow() == 0, "Refill after an empty load reused a stale anchor");
}
void TestRefreshAndSort()
{
	wxFrame frame(nullptr, wxID_ANY, "Virtual list refresh callers", wxDefaultPosition, wxSize(600, 350));
	auto *list = new TestList(&frame, true, true);
	auto *observer = new ChangeObserver;
	list->GetModel()->AddNotifier(observer); // The model owns the notifier.
	frame.Show();
	Settle();
	list->liveSort = true;
	const bool previousLiveSort = CPreferences::LiveListSort();
	TestPreferences::SetLiveSort(true);
	struct RestoreLiveSort
	{
		bool previous;
		~RestoreLiveSort() { TestPreferences::SetLiveSort(previous); }
	} restore{ previousLiveSort };

	// Startup shared-file drains can refresh silent appends before their publication tick.
	for (unsigned i = 1; i <= 200; ++i) {
		list->AppendItemData(i);
	}
	list->RefreshItemData(1);
	list->RefreshItemData(200);
	Check(observer->changes == 0, "Unpublished rows emitted model changes");
	Settle();
	Check(list->comparisons == 0, "Unpublished refresh scheduled a premature live sort");
	Check(list->PublishedCount() == 0, "Refresh published an unfinished bulk load");
	list->FinishBulkLoad();
	Settle();
	Check(list->PublishedCount() == 200 && list->ItemAt(0) == 200,
		"Final bulk completion failed to publish sorted rows");

	// The empty case must not change fallback behavior for an unknown populated viewport.
	list->unknownViewport = true;
	list->RefreshItemData(1);
	Check(observer->changes == 1, "Unknown viewport suppressed a published row update");
	list->unknownViewport = false;
	list->ShowRow(0);
	Settle();
	observer->changes = 0;
	list->RefreshItemData(200);
	Check(observer->changes == 1, "Visible row update was suppressed");
	list->RefreshItemData(1);
	Check(observer->changes == 1, "Off-screen row emitted a model change");
	list->RefreshItemData(999);
	Check(observer->changes == 1, "Missing row emitted a model change");
	list->comparisons = 0;
	Settle();
	Check(list->comparisons > 0, "Published updates no longer trigger live sorting");
	long first = -1, last = -1;
	Check(list->GetVisibleRowRange(first, last), "Visible range unavailable for edge test");
	list->RefreshItemData(list->ItemAt(last + 1));
	Check(observer->changes == 2, "Partially visible edge row was not refreshed");
	observer->changes = 1;

	// Mixed batches may have published rows and a silent tail at the same time.
	list->AppendItemData(201);
	list->unknownViewport = true;
	list->RefreshItemData(201);
	Check(observer->changes == 1, "Unpublished tail row emitted a model change");
	list->unknownViewport = false;
	list->FinishBulkLoad();
	Settle();
	Check(list->PublishedCount() == 201 && list->ItemAt(0) == 201,
		"Mixed batch failed to publish its new row");

	list->Hide();
	list->RefreshItemData(201);
	Check(observer->changes == 1, "Hidden list emitted a model change");
	list->Show();
	list->SetSelectedItemData({ 201, 199 });
	list->SortList();
	const auto selected = list->GetSelectedItemData();
	Check(selected.size() == 2 && list->IsItemDataSelected(201) && list->IsItemDataSelected(199),
		"Sorting changed selected identities");
	list->ClearItemData();
	list->SortList();
	Check(list->PublishedCount() == 0, "Empty sort created rows");
	Settle();
	// Server, friend and file-detail callers publish individual rows immediately.
	list->AddItemData(10);
	list->AppendItemDataNow(20);
	Settle();
	Check(list->PublishedCount() == 2, "Individual additions were not published");
	observer->changes = 0;
	list->RefreshItemData(10);
	list->RefreshItemData(20);
	Check(observer->changes == 2, "Individual published rows stopped refreshing");
	list->RemoveItemData(10);
	list->RefreshItemData(10);
	Check(observer->changes == 2, "Removed item was refreshed");
	Settle();
}

} // namespace

void RunTests()
{
	wxMemoryInputStream input("", 0);
	wxFileConfig config(input);
	wxConfigBase *previous = wxConfigBase::Set(&config);
	struct RestoreConfig
	{
		wxConfigBase *previous;
		~RestoreConfig() { wxConfigBase::Set(previous); }
	} restore{ previous };
	TestRefreshAndSort();
	for (bool generic : { true, false }) {
		for (bool reverse : { true, false }) {
			std::cout << "generic empty viewport=" << generic << ", reverse sort=" << reverse
				  << '\n';
			TestBulkLoad(generic, reverse);
		}
	}
}
