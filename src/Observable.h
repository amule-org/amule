//
// This file is part of the aMule Project.
//
// Copyright (c) 2005-2011 Mikkel Schubert ( xaignar@users.sourceforge.net )
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

#ifndef OBSERVABLE_H
#define OBSERVABLE_H

#include <set>

#include "OtherFunctions.h" // Needed for CMutexUnlocker

template <typename TEST> class CObservable;

/**
 * The observable part of an Observer/Observable pattern.
 *
 * The EventType parameter specifies a protocol for the event type used by a particular
 * Observer/Observable set, allowing any level of information passing, depending on the context.
 *
 * To simplify matters for subclasses, both the Observer and the Observable class track which
 * objects are observing what, so instances can be created and destroyed safely without keeping the
 * observers and observables manually in sync.
 */
template <typename EventType> class CObserver
{
	friend class CObservable<EventType>;

public:
	typedef CObservable<EventType> ObservableType;

	/**
	 * Notifies every observable this object is registered with, to avoid dangling pointers. No
	 * actual events result.
	 */
	virtual ~CObserver();

protected:
	/**
	 * Called when an observed subject publishes an event. @param o The publisher of the event.
	 * @param e The actual event.
	 */
	virtual void ReceiveNotification(const ObservableType *o, const EventType &e) = 0;

private:
	//! Mutex used to make access to the list of observed objects thread safe.
	wxMutex m_mutex;

	typedef std::set<ObservableType *> ObservableSetType;
	//! List of objects being observed.
	ObservableSetType m_list;
};

/**
 * The Observable part of the Observer/Observable pattern.
 */
template <typename EventType> class CObservable
{
	friend class CObserver<EventType>;

public:
	//! The observer-type accepted by this class
	typedef CObserver<EventType> ObserverType;

	virtual ~CObservable();

	/**
	 * Subscribes observer @a o to events from this observable. Returns true on success, in
	 * which case ObserverAdded() is called on @a o so the subclass can initialize its state.
	 */
	bool AddObserver(ObserverType *o);

	/**
	 * Removes observer @a o from the list of subscribers. Returns true if it was removed, in
	 * which case ObserverRemoved() is called on it so the subclass can avoid keeping outdated
	 * data.
	 */
	bool RemoveObserver(ObserverType *o);

protected:
	/**
	 * Notifies all subscribers of event @a e, or just observer @a o when one is given. The
	 * second parameter exists for the notifications ObserverAdded() and ObserverRemoved() make,
	 * and should not be used elsewhere.
	 */
	void NotifyObservers(const EventType &e, ObserverType *o = NULL);

	/**
	 * Removes every observer from this object, calling ObserverRemoved on each.
	 */
	void RemoveAllObservers();

	/**
	 * Called when an observer has been added to the observable.
	 */
	virtual void ObserverAdded(ObserverType *) {};

	/**
	 * Called when observers are removed from the observable, except while the Observable or the
	 * Observer is being destroyed.
	 */
	virtual void ObserverRemoved(ObserverType *) {};

private:
	//! Mutex used to ensure thread-safety of the basic operations.
	wxMutex m_mutex;

	typedef std::set<ObserverType *> ObserverSetType;
	typedef typename ObserverSetType::iterator myIteratorType;

	//! Set of all observers subscribing to this observable.
	ObserverSetType m_list;
};

///////////////////////////////////////////////////////////////////////////////

template <typename EventType> CObserver<EventType>::~CObserver()
{
	wxMutexLocker lock(m_mutex);

	while (!m_list.empty()) {
		ObservableType *o = *m_list.begin();

		{
			wxMutexLocker oLock(o->m_mutex);
			o->m_list.erase(this);
		}

		m_list.erase(m_list.begin());
	}
}

template <typename EventType> CObservable<EventType>::~CObservable()
{
	wxMutexLocker lock(m_mutex);

	while (!m_list.empty()) {
		ObserverType *o = *m_list.begin();

		{
			wxMutexLocker oLock(o->m_mutex);
			o->m_list.erase(this);
		}

		m_list.erase(m_list.begin());
	}
}

template <typename EventType> bool CObservable<EventType>::AddObserver(CObserver<EventType> *o)
{
	wxASSERT(o);

	{
		wxMutexLocker lock(m_mutex);
		if (!m_list.insert(o).second) {
			return false;
		}
	}

	{
		wxMutexLocker oLock(o->m_mutex);
		o->m_list.insert(this);
	}

	ObserverAdded(o);

	return true;
}

template <typename EventType> bool CObservable<EventType>::RemoveObserver(CObserver<EventType> *o)
{
	wxASSERT(o);

	{
		wxMutexLocker lock(m_mutex);
		if (!m_list.erase(o)) {
			return false;
		}
	}

	{
		wxMutexLocker oLock(o->m_mutex);
		o->m_list.erase(this);
	}

	ObserverRemoved(o);

	return true;
}

template <typename EventType>
void CObservable<EventType>::NotifyObservers(const EventType &e, ObserverType *o)
{
	wxMutexLocker lock(m_mutex);

	if (o) {
		o->ReceiveNotification(this, e);
	} else {
		myIteratorType it = m_list.begin();
		for (; it != m_list.end();) {
			CMutexUnlocker unlocker(m_mutex);
			(*it++)->ReceiveNotification(this, e);
		}
	}
}

template <typename EventType> void CObservable<EventType>::RemoveAllObservers()
{
	wxMutexLocker lock(m_mutex);

	while (!m_list.empty()) {
		ObserverType *o = *m_list.begin();
		m_list.erase(m_list.begin());
		CMutexUnlocker unlocker(m_mutex);

		{
			wxMutexLocker oLock(o->m_mutex);
			o->m_list.erase(this);
		}

		ObserverRemoved(o);
	}
}

#endif
// File_checked_for_headers
