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
#include <chrono>
#include <vector>
#include <wx/thread.h>

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
	: CThreadTask("Running", "blocked", ETP_High)
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
	COrderedTask(std::vector<int> &order,
		wxSemaphore &done,
		int value,
		ETaskPriority priority,
		const wxString &description = wxEmptyString)
	: CThreadTask("Order", description.IsEmpty() ? wxString::Format("%d", value) : description, priority)
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

// Poll cancellation concurrently, then hold the worker so the replacement stays queued.
class CCancelledTask : public CThreadTask
{
public:
	CCancelledTask(wxSemaphore &started, wxSemaphore &release, wxSemaphore &done, bool &cancelled)
	: CThreadTask("Order", "running-verify", ETP_Low)
	, m_started(started)
	, m_release(release)
	, m_done(done)
	, m_cancelled(cancelled)
	{
	}

protected:
	void Entry() override
	{
		m_started.Post();
		while (!TestDestroy()) {
			wxThread::Sleep(1);
		}
		m_cancelled = true;
		m_release.Wait();
		m_done.Post();
	}

private:
	wxSemaphore &m_started;
	wxSemaphore &m_release;
	wxSemaphore &m_done;
	bool &m_cancelled;
};

// Stay in Entry until shutdown, leaving queued tasks for the scheduler to release.
class CStopTask : public CThreadTask
{
public:
	explicit CStopTask(wxSemaphore &started)
	: CThreadTask("Stopping", "shutdown", ETP_High)
	, m_started(started)
	{
	}

protected:
	void Entry() override
	{
		m_started.Post();
		while (!TestDestroy()) {
			wxThread::Sleep(1);
		}
	}

private:
	wxSemaphore &m_started;
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
	ASSERT_TRUE(CThreadScheduler::AddTask(new CCountedTask(), CThreadScheduler::Overwrite::Always));

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
	bool cancelled = false;
	CReleaseWorker cleanup(release);
	ASSERT_TRUE(CThreadScheduler::AddTaskIfNoneOfType(new CBlockedTask(started, release)));
	CThreadScheduler::Start();
	ASSERT_EQUALS(wxSEMA_NO_ERROR, started.WaitTimeout(5000));
	// The running task has left the queue but must still block another of its type.
	ASSERT_EQUALS(static_cast<size_t>(1), CThreadScheduler::GetPendingCount("Running"));
	ASSERT_TRUE(CThreadScheduler::HasTaskOfType("Running"));
	const bool queued = CThreadScheduler::AddTaskIfNoneOfType(new CCountedTask("other", "Running"));
	ASSERT_FALSE(queued);
	// A manual Verify replaces an automatic check of the same file while the worker is
	// busy. Its high priority and manual reporting metadata (value 4) must both survive.
	ASSERT_TRUE(CThreadScheduler::AddTask(new COrderedTask(order, done, 3, ETP_Low, "verify-file")));
	ASSERT_TRUE(CThreadScheduler::AddTask(new COrderedTask(order, done, 4, ETP_High, "verify-file"),
		CThreadScheduler::Overwrite::LowerPriority));
	ASSERT_EQUALS(static_cast<size_t>(1), CThreadScheduler::GetPendingCount("Order"));
	// Repeating manual Verify must preserve the queued manual task and its reporting metadata.
	ASSERT_FALSE(CThreadScheduler::AddTask(new COrderedTask(order, done, 5, ETP_High, "verify-file"),
		CThreadScheduler::Overwrite::LowerPriority));
	ASSERT_FALSE(CThreadScheduler::AddTask(new COrderedTask(order, done, 6, ETP_Low, "verify-file"),
		CThreadScheduler::Overwrite::LowerPriority));
	// A running task also keeps its work when another request has equal priority.
	ASSERT_FALSE(CThreadScheduler::AddTask(
		new CBlockedTask(started, release), CThreadScheduler::Overwrite::LowerPriority));
	// New normal-priority hashing must pass an older opportunistic low-priority check.
	ASSERT_TRUE(CThreadScheduler::AddTask(new COrderedTask(order, done, 1, ETP_Low)));
	ASSERT_TRUE(CThreadScheduler::AddTask(new COrderedTask(order, done, 2, ETP_Normal)));
	// Keep this worker alive for the cancellation phase instead of racing idle-thread exit.
	ASSERT_TRUE(CThreadScheduler::AddTask(new CCancelledTask(started, release, done, cancelled)));
	release.Post();
	ASSERT_EQUALS(wxSEMA_NO_ERROR, done.WaitTimeout(5000));
	ASSERT_EQUALS(wxSEMA_NO_ERROR, done.WaitTimeout(5000));
	ASSERT_EQUALS(wxSEMA_NO_ERROR, done.WaitTimeout(5000));
	ASSERT_TRUE((order == std::vector<int>{ 4, 2, 1 }));

	ASSERT_EQUALS(wxSEMA_NO_ERROR, started.WaitTimeout(5000));
	ASSERT_TRUE(CThreadScheduler::AddTask(new COrderedTask(order, done, 7, ETP_High, "running-verify"),
		CThreadScheduler::Overwrite::LowerPriority));
	ASSERT_FALSE(CThreadScheduler::AddTask(new COrderedTask(order, done, 8, ETP_High, "running-verify"),
		CThreadScheduler::Overwrite::LowerPriority));
	ASSERT_TRUE(CThreadScheduler::AddTask(new CStopTask(started)));
	release.Post();
	ASSERT_EQUALS(wxSEMA_NO_ERROR, done.WaitTimeout(5000));
	ASSERT_EQUALS(wxSEMA_NO_ERROR, done.WaitTimeout(5000));
	ASSERT_TRUE(cancelled);
	ASSERT_TRUE((order == std::vector<int>{ 4, 2, 1, 7 }));
	ASSERT_EQUALS(wxSEMA_NO_ERROR, started.WaitTimeout(5000));
	ASSERT_TRUE(CThreadScheduler::AddTask(new CCountedTask("shutdown-pending")));
	ASSERT_EQUALS(1, s_liveTasks);

	CThreadScheduler::Terminate();
	ASSERT_EQUALS(0, s_liveTasks);
	ASSERT_FALSE(CThreadScheduler::HasTaskOfType("Running"));
	ASSERT_FALSE(CThreadScheduler::AddTaskIfNoneOfType(new CCountedTask()));
	ASSERT_EQUALS(0, s_liveTasks);
}

// A task added while the idle worker is exiting must still run. Each round queues the next task
// right after the previous one finishes, when the worker is about to find the queue empty.
TEST(ThreadScheduler, TaskAddedWhileIdleWorkerExitsStillRuns)
{
	wxSemaphore done;
	std::vector<int> order;
	CThreadScheduler::Start();
	for (int round = 0; round < 10000; ++round) {
		ASSERT_TRUE(CThreadScheduler::AddTask(new COrderedTask(order, done, round, ETP_Normal)));
		ASSERT_EQUALS(wxSEMA_NO_ERROR, done.WaitTimeout(5000));
		// Sweep the gap so some rounds land after the worker has released the lock.
		const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(round % 200);
		while (std::chrono::steady_clock::now() < until) {
		}
	}
	CThreadScheduler::Terminate();
	ASSERT_EQUALS(static_cast<size_t>(10000), order.size());
}
