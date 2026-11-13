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
	bool IsLiveSortColumn() const override { return true; }
	unsigned PublishedCount() const { return Model()->GetCount(); }
	void ShowRow(unsigned row) { EnsureVisible(Model()->GetItem(row)); }
	long TopRow() const
	{
		long first = -1, last = -1;
		Check(GetVisibleRowRange(first, last), "Populated list has no visible range");
		return first;
	}
	void Load(unsigned count)
	{
		Freeze();
		ClearItemData();
		for (unsigned i = 1; i <= count; ++i) {
			AppendItemData(i);
		}
		FinishBulkLoad();
		Thaw();
	}
	using CMuleVirtualDataViewCtrl::ItemAt;
	bool unknownViewport = false;
	mutable unsigned comparisons = 0;

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

void Settle(wxTopLevelWindow &frame, const TestList &list)
{
	// GTK may scroll over several frames: settle on one that leaves the rows in place.
	long first = -1, last = -1;
	for (int frames = 0; frames < 10; ++frames) {
		WaitForPaint(frame);
		long newFirst = -1, newLast = -1;
		list.GetVisibleRowRange(newFirst, newLast);
		if (frames > 0 && newFirst == first && newLast == last) {
			break;
		}
		first = newFirst;
		last = newLast;
	}
	for (int i = 0; i < 10; ++i) {
		wxTheApp->Yield(true);
		wxMilliSleep(10);
	}
}

// Issue #2439: a sorted startup load opened the download list pages down on Windows.
void TestBulkLoad(bool genericEmptyViewport, bool reverse)
{
	wxFrame frame(nullptr, wxID_ANY, "Bulk load scroll test", wxDefaultPosition, wxSize(600, 350));
	auto *list = new TestList(&frame, genericEmptyViewport, reverse);
	frame.Show();
	Settle(frame, *list);
	list->Load(200);
	Settle(frame, *list);
	Check(list->TopRow() == 0, "Initial bulk load scrolled away from the top");
	Check(list->ItemAt(0) == (reverse ? 200 : 1), "Initial sort order is wrong");

	// A displayed scroll position must survive a clear and refill.
	list->ShowRow(100);
	Settle(frame, *list);
	Check(list->TopRow() > 0, "Fixture did not scroll the populated list");
	const wxUIntPtr anchor = list->ItemAt(list->TopRow());
	list->Load(200);
	Settle(frame, *list);
	// GTK may leave a fraction of the preceding row visible after EnsureVisible.
	Check(list->ItemAt(list->TopRow()) == anchor || list->ItemAt(list->TopRow() + 1) == anchor,
		"Rebuild discarded the existing scroll anchor");

	// An empty load must not leave an anchor behind for the next one.
	list->Load(0);
	Settle(frame, *list);
	list->Load(200);
	Settle(frame, *list);
	Check(list->TopRow() == 0, "Refill after an empty load reused a stale anchor");
}

// Rows appended silently are not in the control's model until FinishBulkLoad().
void TestUnpublishedRefresh()
{
	wxFrame frame(nullptr, wxID_ANY, "Unpublished refresh test", wxDefaultPosition, wxSize(600, 350));
	auto *list = new TestList(&frame, true, true);
	auto *observer = new ChangeObserver;
	list->GetModel()->AddNotifier(observer); // The model owns the notifier.
	frame.Show();
	Settle(frame, *list);
	// Restored on unwind too: a failed Check() throws.
	struct RestoreLiveSort
	{
		bool previous = CPreferences::LiveListSort();
		~RestoreLiveSort() { TestPreferences::SetLiveSort(previous); }
	} restoreLiveSort;
	TestPreferences::SetLiveSort(true);

	for (unsigned i = 1; i <= 200; ++i) {
		list->AppendItemData(i);
	}
	list->RefreshItemData(1);
	list->RefreshItemData(200);
	Settle(frame, *list);
	Check(observer->changes == 0, "Unpublished rows emitted model changes");
	Check(list->comparisons == 0, "Unpublished refresh scheduled a live sort");
	list->FinishBulkLoad();
	Settle(frame, *list);
	Check(list->PublishedCount() == 200, "Bulk completion lost rows");

	// A tail appended to a populated list, even when the viewport is unknown.
	list->AppendItemData(201);
	list->unknownViewport = true;
	list->RefreshItemData(201);
	list->unknownViewport = false;
	Check(observer->changes == 0, "Unpublished tail row emitted a model change");
	list->FinishBulkLoad();
	Settle(frame, *list);
	Check(list->PublishedCount() == 201, "Bulk completion lost the tail row");
}

} // namespace

void RunTests()
{
	wxMemoryInputStream input("", 0);
	wxFileConfig config(input);
	// wx deletes the global config at cleanup, so restore it even when a Check() throws.
	struct RestoreConfig
	{
		wxConfigBase *previous;
		~RestoreConfig() { wxConfigBase::Set(previous); }
	} restoreConfig{ wxConfigBase::Set(&config) };
	for (bool generic : { true, false }) {
		for (bool reverse : { true, false }) {
			std::cout << "generic empty viewport=" << generic << ", reverse sort=" << reverse
				  << '\n';
			TestBulkLoad(generic, reverse);
		}
	}
	TestUnpublishedRefresh();
}
