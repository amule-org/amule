//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//

#ifndef THROTTLERWAKE_H
#define THROTTLERWAKE_H

#include <wx/thread.h>

#include "Types.h"

// Wakes a sleeping thread. A wake sent while the thread is busy stays pending, so its next Wait()
// returns at once instead of sleeping through it.
class CThrottlerWake
{
public:
	CThrottlerWake()
	: m_condition(m_mutex)
	{
	}

	void Notify()
	{
		wxMutexLocker lock(m_mutex);
		m_pending = true;
		m_condition.Signal();
	}

	// Sleeps up to timeoutMs, or not at all when a wake is pending. Consumes the wake and returns
	// whether there was one.
	bool Wait(uint64 timeoutMs)
	{
		wxMutexLocker lock(m_mutex);
		if (!m_pending && timeoutMs > 0) {
			// A spurious wakeup only runs the caller's loop early.
			m_condition.WaitTimeout(static_cast<unsigned long>(timeoutMs));
		}
		const bool woken = m_pending;
		m_pending = false;
		return woken;
	}

private:
	wxMutex m_mutex;
	wxCondition m_condition;
	bool m_pending = false;
};

#endif
// File_checked_for_headers
