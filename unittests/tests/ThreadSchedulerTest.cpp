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

#include <muleunit/test.h>

#include "ThreadScheduler.h"
#include <wx/thread.h>
#include <vector>

using namespace muleunit;

DECLARE_SIMPLE(ThreadScheduler)

namespace
{
int s_liveTasks = 0;

class CCountedTask : public CThreadTask
{
public:
	CCountedTask(const wxString &desc = "same", const wxString &type = "Counted")
	: CThreadTask(type, desc)
	{
		++s_liveTasks;
	}

	~CCountedTask() override { --s_liveTasks; }

protected:
	void Entry() override {}
};

class CBlockedTask : public CThreadTask
{
public:
	CBlockedTask(wxSemaphore &started, wxSemaphore &release)
	: CThreadTask("Running", "blocked")
	, m_started(started)
	, m_release(release)
	{
	}

protected:
	void Entry() override
	{
		m_started.Post();
		m_release.Wait();
	}

private:
	wxSemaphore &m_started;
	wxSemaphore &m_release;
};

class COrderedTask : public CThreadTask
{
public:
	COrderedTask(std::vector<int> &order, wxSemaphore &done, int value, ETaskPriority priority)
	: CThreadTask("Order", wxString::Format("%d", value), priority)
	, m_order(order)
	, m_done(done)
	, m_value(value)
	{
	}

protected:
	void Entry() override
	{
		m_order.push_back(m_value);
		m_done.Post();
	}

private:
	std::vector<int> &m_order;
	wxSemaphore &m_done;
	int m_value;
};

// Always release the worker if an assertion unwinds the test.
class CReleaseWorker
{
public:
	explicit CReleaseWorker(wxSemaphore &release)
	: m_release(release)
	{
	}
	~CReleaseWorker()
	{
		m_release.Post();
		CThreadScheduler::Terminate();
	}

private:
	wxSemaphore &m_release;
};
} // namespace

// Start suspended to check queued tasks first, then hold a live task at a semaphore.
TEST(ThreadScheduler, QueueOwnershipAndTypeLimitsAcrossSchedulerLifecycle)
{
	ASSERT_TRUE(CThreadScheduler::AddTask(new CCountedTask()));
	ASSERT_TRUE(CThreadScheduler::AddTask(new CCountedTask(), true));

	ASSERT_EQUALS(1, s_liveTasks);
	// An empty type never dereferences entries, so a dangling one is still counted.
	ASSERT_EQUALS(static_cast<size_t>(1), CThreadScheduler::GetPendingCount());

	// Different file descriptions still share the one-per-type limit. Rejected objects die.
	ASSERT_FALSE(CThreadScheduler::AddTaskIfNoneOfType(new CCountedTask("other")));
	ASSERT_EQUALS(1, s_liveTasks);
	ASSERT_TRUE(CThreadScheduler::AddTaskIfNoneOfType(new CCountedTask("first", "Automatic")));
	ASSERT_FALSE(CThreadScheduler::AddTaskIfNoneOfType(new CCountedTask("second", "Automatic")));
	ASSERT_EQUALS(2, s_liveTasks);
	ASSERT_EQUALS(static_cast<size_t>(1), CThreadScheduler::GetPendingCount("Automatic"));
	ASSERT_TRUE(CThreadScheduler::HasTaskOfType("Automatic"));
	ASSERT_FALSE(CThreadScheduler::HasTaskOfType("Absent"));
	wxSemaphore started, release, done;
	std::vector<int> order;
	CReleaseWorker cleanup(release);
	ASSERT_TRUE(CThreadScheduler::AddTaskIfNoneOfType(new CBlockedTask(started, release)));
	CThreadScheduler::Start();
	ASSERT_EQUALS(wxSEMA_NO_ERROR, started.WaitTimeout(5000));
	// The running task has left the queue but must still block another of its type.
	ASSERT_EQUALS(static_cast<size_t>(1), CThreadScheduler::GetPendingCount("Running"));
	ASSERT_TRUE(CThreadScheduler::HasTaskOfType("Running"));
	const bool queued = CThreadScheduler::AddTaskIfNoneOfType(new CCountedTask("other", "Running"));
	ASSERT_FALSE(queued);
	// New normal-priority hashing must pass an older opportunistic low-priority check.
	ASSERT_TRUE(CThreadScheduler::AddTask(new COrderedTask(order, done, 1, ETP_Low)));
	ASSERT_TRUE(CThreadScheduler::AddTask(new COrderedTask(order, done, 2, ETP_Normal)));
	release.Post();
	ASSERT_EQUALS(wxSEMA_NO_ERROR, done.WaitTimeout(5000));
	ASSERT_EQUALS(wxSEMA_NO_ERROR, done.WaitTimeout(5000));
	ASSERT_TRUE((order == std::vector<int>{ 2, 1 }));

	CThreadScheduler::Terminate();
	ASSERT_EQUALS(0, s_liveTasks);
	ASSERT_FALSE(CThreadScheduler::HasTaskOfType("Running"));
	ASSERT_FALSE(CThreadScheduler::AddTaskIfNoneOfType(new CCountedTask()));
	ASSERT_EQUALS(0, s_liveTasks);
}
