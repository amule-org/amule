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

#ifndef AMULE_NATIVE_LIST_TEST_H
#define AMULE_NATIVE_LIST_TEST_H

#include <vector>

#include <wx/dataview.h>
#include <wx/fileconf.h>
#include <wx/mstream.h>

#include "Preferences.h"

void RunTests();

// An empty in-memory config. wx deletes the global config at cleanup, so the
// old one comes back even when a Check() throws.
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

// Turns the live-sort preference on and restores it on unwind.
struct LiveSortScope
{
	LiveSortScope()
	: previous(CPreferences::LiveListSort())
	{
		Set(true);
	}
	~LiveSortScope() { Set(previous); }
	static void Set(bool enabled) { Access::s_liveListSort = enabled; }
	bool previous;

private:
	struct Access : CPreferences
	{
		using CPreferences::s_liveListSort;
	};
};

// Records the rows the model reports as changed, and full resets.
class ModelObserver : public wxDataViewModelNotifier
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

#endif // AMULE_NATIVE_LIST_TEST_H
