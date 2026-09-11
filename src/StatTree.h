//
// This file is part of the aMule Project.
//
// Copyright (c) 2003-2026 aMule Team ( https://amule-org.github.io )
// Copyright (c) 2005-2011 Dévai Tamás ( gonosztopi@amule.org )
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

#ifndef STATTREE_H
#define STATTREE_H

/**
 * @file StatTree.h
 *
 * Classes representing statistics tree nodes.
 */

#ifndef CLIENT_GUI
#define VIRTUAL virtual
#else
#define VIRTUAL
#endif

#include <list>        // Needed for std::list
#include <wx/string.h> // Needed for wxString
#include <wx/thread.h> // Needed for wxMutex
#include "Types.h"

#ifndef CLIENT_GUI

#include <wx/datetime.h>  // Needed for wxDateTime
#include "GetTickCount.h" // Needed for GetTickCount64()

/** Stat tree flags */
enum EStatTreeFlags
{
	stNone = 0,         ///< Nothing. Really.
	stSortChildren = 1, ///< Childrens are sorted descending by their ID.
	stShowPercent = 2,  /*!< Shows percentage compared to parent.
			     *   Counters only, whose parent is also counter! (khmm...)
			     */
	stHideIfZero = 4,   ///< Hides item (and children) if value is zero.
	stSortByValue =
		8,         /*!< Together with stSortChildren, sorts children by their value.
			    *   WARNING! This assumes that value-sorted children are
			    *   counters!
			    *   Sort-by-value works only on children with ID between 0x00000100-0x7fffffff.
			    *   @note CStatTreeItemBase::ReSortChildren() must be called to get sort order right.
			    */
	stCapChildren = 16 ///< Caps children list.
			   /*!< Shows only the top n children, where n is set by
			    * CStatisticsDlg::FillTree() to thePrefs::GetMaxClientVersions(). The list
			    * itself is not changed, only visibility. On an EC request the range comes
			    * from the EC_TAG_STATTREE_CAPPING tag.
			    */
};

enum EValueType
{
	vtUnknown,
	vtString,
	vtInteger,
	vtFloat
};

/** Display modes for Simple and Counter items */
enum EDisplayMode
{
	dmDefault, ///< Default display mode.
	dmTime,    ///< Treat integer value as time in seconds.
	dmBytes    ///< Treat integer value as bytes count.
};

#endif /* !CLIENT_GUI */

class CStatTreeItemBase;
typedef std::list<CStatTreeItemBase *>::iterator StatTreeItemIterator;

class CECTag;

uint32_t NewStatTreeItemId();

/** Base tree item class */
class CStatTreeItemBase
{
public:
#ifndef CLIENT_GUI
	/**
	 * Creates an item with a constant label.
	 *
	 * @param label Visible text for the item.
	 * @param flags Flags to use.
	 */
	CStatTreeItemBase(const wxString &label, unsigned flags = stNone)
	: m_label(label)
	, m_parent(NULL)
	, m_flags(flags)
	, m_id(0)
	, m_visible_counter(0)
	, m_uniqueid(NewStatTreeItemId())
	{
	}
#else
	/** Creates an item with a constant label. */
	CStatTreeItemBase(const wxString &label, uint32_t uniqueid)
	: m_label(label)
	, m_uniqueid(uniqueid)
	{
	}

	/** Creates an item (and the whole subtree) from an EC tag. */
	CStatTreeItemBase(const CECTag *tag);
#endif

	VIRTUAL ~CStatTreeItemBase();

#ifndef CLIENT_GUI
	/**
	 * Adds a new child node and returns it.
	 *
	 * @param skipOneLevel activates a trick to let the code work for aMule OSInfo and
	 * Version trees.
	 */
	CStatTreeItemBase *AddChild(CStatTreeItemBase *child, uint32_t id = 0, bool skipOneLevel = false);

	/**
	 * Assigns a stable, untranslated machine key to this node, emitted over EC
	 * (EC_TAG_STAT_NODE_KEY) so API clients can look a field up by a fixed identifier
	 * instead of matching the translated/formatted label. An empty key means "no key"
	 * and omits the tag. Returns this node, so the call can be chained onto a
	 * freshly-constructed item: AddChild(new CStatTreeItemFoo(...)->SetKey("foo")).
	 */
	CStatTreeItemBase *SetKey(const wxString &key)
	{
		m_key = key;
		return this;
	}

	const wxString &GetKey() const { return m_key; }

	/**
	 * Attaches a raw, untranslated machine value for a node whose label is data (a
	 * client version or OS string). Serialized as EC_TAG_STAT_NODE_RAW so API clients
	 * need not parse it out of the composite label. Empty means "none" and omits the
	 * tag. Returns this node, so the call can be chained.
	 */
	CStatTreeItemBase *SetRawValue(const wxString &raw)
	{
		m_rawvalue = raw;
		return this;
	}

	const wxString &GetRawValue() const { return m_rawvalue; }
#endif

	bool HasChildren()
	{
		wxMutexLocker lock(m_lock);
		return !m_children.empty();
	}

	/** @return true if this node has children and at least one of them is visible. */
	bool HasVisibleChildren();

#ifndef CLIENT_GUI

	/** @return true if this node has a child with the given ID. */
	bool HasChildWithId(uint32_t id);

	/** @return the child with the given ID, or NULL if not found. */
	CStatTreeItemBase *GetChildById(uint32_t id);

	/**
	 * Get the first visible child.
	 *
	 * @param max_children The maximum number of children to show when the stCapChildren
	 * flag is set; no effect otherwise (0 = unlimited).
	 * @return An iterator, to be passed to GetNextVisibleChild() and IsAtEndOfList().
	 */
	StatTreeItemIterator GetFirstVisibleChild(uint32_t max_children);

	void GetNextVisibleChild(StatTreeItemIterator &it);

#else /* CLIENT_GUI */

	/**
	 * @return An iterator, to be passed to GetNextVisibleChild() and IsAtEndOfList().
	 * @note On a remote list every item is visible.
	 */
	StatTreeItemIterator GetFirstVisibleChild() { return m_children.begin(); }

	/** @note On a remote list every item is visible. */
	void GetNextVisibleChild(StatTreeItemIterator &it) { ++it; }

#endif /* !CLIENT_GUI / CLIENT_GUI */

	/** Check if we are past the end of the child list. */
	bool IsAtEndOfList(StatTreeItemIterator &it) { return it == m_children.end(); }

#ifndef CLIENT_GUI
	/** Resorts children for the stSortByValue flag. */
	void ReSortChildren()
	{
		wxMutexLocker lock(m_lock);
		m_children.sort(ValueSort);
	}
#endif

#ifndef AMULE_DAEMON
#ifndef CLIENT_GUI
	/** Returns a string that will be displayed on the GUI tree. */
	virtual wxString GetDisplayString() const;
#else
	/** Returns the associated text (GUI item label). */
	const wxString &GetDisplayString() const { return m_label; }
#endif /* !CLIENT_GUI / CLIENT_GUI */

	/**
	 * Returns the mutex locking this node's child list, so CStatisticsDlg can lock the
	 * core tree while updating the GUI tree.
	 */
	wxMutex &GetLock() { return m_lock; }

	/** Returns the unique ID of this node. */
	uint32_t GetUniqueId() const { return m_uniqueid; }
#endif /* !AMULE_DAEMON */

	VIRTUAL bool IsVisible() const { return true; }

#ifndef CLIENT_GUI
	/**
	 * Create an EC tag from this node and its children.
	 *
	 * @param max_children The maximum number of children to show when the stCapChildren
	 * flag is set; no effect otherwise (0 = unlimited).
	 */
	virtual CECTag *CreateECTag(uint32_t max_children);
#endif

protected:
#ifndef CLIENT_GUI
	/**
	 * Add values to the EC tag being generated. Children that have a value give this a
	 * real implementation.
	 */
	virtual void AddECValues(CECTag *) const {}
#endif

	//! Unformatted and untranslated label of the node. Note: On remote gui it is already formatted and
	//! translated.
	const wxString m_label;
#ifndef CLIENT_GUI

	CStatTreeItemBase *m_parent;

	unsigned m_flags;

	//! Stable, untranslated machine key (empty = none). @see SetKey
	wxString m_key;

	//! Raw untranslated machine value for data-labelled nodes (empty = none).
	//! @see SetRawValue
	wxString m_rawvalue;
#endif

private:
#ifndef CLIENT_GUI
	static bool ValueSort(const CStatTreeItemBase *a, const CStatTreeItemBase *b);

	uint32_t m_id;

	//! Counter to keep track of displayed visible items
	// (needed for the stCapChildren flag)
	uint32_t m_visible_counter;
#endif

	uint32_t m_uniqueid;

	std::list<CStatTreeItemBase *> m_children;

	wxMutex m_lock;
};

// Anything below is only for core.
#ifndef CLIENT_GUI

/**
 * Simple tree item: one value, of an arbitrary integer or floating point type, or a
 * wxString. It can display the value in several formats, see SetDisplayMode().
 *
 * @note The format code on 'label' has to match: %s for a string and for integers
 * with displayMode dmTime or dmBytes, %u or similar for integers, %f or similar for
 * floating point types.
 *
 * @note Call SetValue() after creation for non-integer values, or the results are
 * undesired.
 */
class CStatTreeItemSimple : public CStatTreeItemBase
{
public:
	CStatTreeItemSimple(
		const wxString &label, unsigned flags = stNone, enum EDisplayMode displaymode = dmDefault)
	: CStatTreeItemBase(label, flags)
	, m_valuetype(vtUnknown)
	, m_displaymode(displaymode)
	{
		SetValue((uint64_t)0);
	}

	/** Sets the desired display mode of value. */
	void SetDisplayMode(enum EDisplayMode mode) { m_displaymode = mode; }

	/** Sets an integer type value. */
	void SetValue(uint64_t value)
	{
		m_valuetype = vtInteger;
		m_intvalue = value;
	}

	/** Sets a floating point type value. */
	void SetValue(double value)
	{
		m_valuetype = vtFloat;
		m_floatvalue = value;
	}

	/** Sets a string type value. */
	void SetValue(const wxString &value)
	{
		m_valuetype = vtString;
		m_stringvalue = value;
	}

#ifndef AMULE_DAEMON
	virtual wxString GetDisplayString() const;
#endif

	virtual bool IsVisible() const;

protected:
	virtual void AddECValues(CECTag *tag) const;

	enum EValueType m_valuetype;
	enum EDisplayMode m_displaymode;
	union
	{
		uint64_t m_intvalue; ///< Integer value.
		double m_floatvalue; ///< Floating point value.
	};
	wxString m_stringvalue; ///< String value.
};

/**
 * Counter-type tree item template. Able to show a percentage compared to its parent
 * and to hide itself when the value is zero, plus convenience operators for changing
 * the value. The stShowPercent and stHideIfZero flags take effect only on this node.
 */
template <typename _Tp> class CStatTreeItemCounterTmpl : public CStatTreeItemBase
{
public:
	CStatTreeItemCounterTmpl(const wxString &label, unsigned flags = stNone)
	: CStatTreeItemBase(label, flags)
	, m_value(0)
	, m_displaymode(dmDefault)
	{
	}

	/** Retrieve counter value. */
	_Tp GetValue() const { return m_value; }

	operator _Tp() const { return m_value; }

	/** Set counter to given value. */
	void SetValue(_Tp value) { m_value = value; }

	void operator=(_Tp value) { m_value = value; }

	/** Increase value by 1. */
	void operator++() { ++m_value; }

	/** Decrease value by 1. */
	void operator--() { --m_value; }

	/** Increase value by given amount. */
	void operator+=(_Tp value) { m_value += value; }

	/** Decrease value by given amount. */
	void operator-=(_Tp value) { m_value -= value; }

	/** Sets the desired display mode of value. */
	void SetDisplayMode(enum EDisplayMode mode) { m_displaymode = mode; }

#ifndef AMULE_DAEMON
	virtual wxString GetDisplayString() const;
#endif

	virtual bool IsVisible() const { return (m_flags & stHideIfZero) ? (m_value != 0) : true; }

protected:
	virtual void AddECValues(CECTag *tag) const;

	_Tp m_value;

	enum EDisplayMode m_displaymode;
};

typedef CStatTreeItemCounterTmpl<uint64_t> CStatTreeItemCounter;
typedef CStatTreeItemCounterTmpl<uint32_t> CStatTreeItemNativeCounter;

/** A counter that does not display its value. */
class CStatTreeItemHiddenCounter : public CStatTreeItemCounter
{
public:
	CStatTreeItemHiddenCounter(const wxString &label, unsigned flags = stNone)
	: CStatTreeItemCounter(label, flags)
	{
	}

#ifndef AMULE_DAEMON
	// Deliberately calls the grandparent: CStatTreeItemHiddenCounter shows only the
	// base label and hides the parent CStatTreeItemCounter's count display.
	// NOLINTNEXTLINE(bugprone-parent-virtual-call)
	virtual wxString GetDisplayString() const { return CStatTreeItemBase::GetDisplayString(); }
#endif

	virtual bool IsVisible() const { return true; }

protected:
	virtual void AddECValues(CECTag *) const {}
};

/** Item for the session/total upload/download counter */
class CStatTreeItemUlDlCounter : public CStatTreeItemCounter
{
public:
	/**
	 * @param label     format text for item.
	 * @param totalfunc function that will return the totals.
	 */
	CStatTreeItemUlDlCounter(const wxString &label, uint64_t (*totalfunc)(), unsigned flags = stNone)
	: CStatTreeItemCounter(label, flags)
	, m_totalfunc(totalfunc)
	{
	}

#ifndef AMULE_DAEMON
	virtual wxString GetDisplayString() const;
#endif

protected:
	virtual void AddECValues(CECTag *tag) const;

	//! A function whose return value is the total (without current) value.
	uint64_t (*m_totalfunc)();
};

/**
 * Counter-like tree item which remembers its max value. Used for the active
 * connections counter, to get peak connections.
 */
class CStatTreeItemCounterMax : public CStatTreeItemBase
{
public:
	CStatTreeItemCounterMax(const wxString &label)
	: CStatTreeItemBase(label)
	, m_value(0)
	, m_max_value(0)
	{
	}

	/** Increase value */
	void operator++()
	{
		if (++m_value > m_max_value) {
			m_max_value = m_value;
		}
	}

	/** Decrease value */
	void operator--() { --m_value; }

	/** Retrieve actual value */
	uint32_t GetValue() { return m_value; }

	/** Retrieve max value */
	uint32_t GetMaxValue() { return m_max_value; }

#ifndef AMULE_DAEMON
	virtual wxString GetDisplayString() const;
#endif

protected:
	virtual void AddECValues(CECTag *tag) const;

	uint32_t m_value;

	uint32_t m_max_value;
};

/** Tree item for counting packets */
class CStatTreeItemPackets : public CStatTreeItemBase
{
	friend class CStatTreeItemPacketTotals;

public:
	CStatTreeItemPackets(const wxString &label)
	: CStatTreeItemBase(label, stNone)
	, m_packets(0)
	, m_bytes(0)
	{
	}

	/** Add a packet of size 'size'. */
	void operator+=(long size)
	{
		++m_packets;
		m_bytes += size;
	}

#ifndef AMULE_DAEMON
	virtual wxString GetDisplayString() const;
#endif

protected:
	virtual void AddECValues(CECTag *tag) const;

	uint32_t m_packets;

	uint64_t m_bytes;
};

/**
 * Tree item for counting totals on packet counters: sums a number of packet counters
 * and adds its own values.
 */
class CStatTreeItemPacketTotals : public CStatTreeItemPackets
{
public:
	CStatTreeItemPacketTotals(const wxString &label)
	: CStatTreeItemPackets(label)
	{
	}

	/** Adds a packet counter whose values should be counted in the totals. */
	void AddPacketCounter(CStatTreeItemPackets *counter) { m_counters.push_back(counter); }

#ifndef AMULE_DAEMON
	virtual wxString GetDisplayString() const;
#endif

protected:
	virtual void AddECValues(CECTag *tag) const;

	std::vector<CStatTreeItemPackets *> m_counters;
};

/** Tree item for timer type nodes. */
class CStatTreeItemTimer : public CStatTreeItemBase
{
public:
	CStatTreeItemTimer(const wxString &label, unsigned flags = stNone)
	: CStatTreeItemBase(label, flags)
	, m_value(0)
	{
	}

	/** Sets timer start time (and thus starts the timer). */
	void SetStartTime(uint64_t value) { m_value = value; }

	/** Starts the timer if it is not running. */
	void StartTimer()
	{
		if (!m_value)
			m_value = GetTickCount64();
	}

	/** Stops the timer. */
	void StopTimer() { m_value = 0; }

	/** Check whether the timer is running. */
	bool IsRunning() const { return m_value != 0; }

	/** Reset timer unconditionally. */
	void ResetTimer() { m_value = GetTickCount64(); }

	/** Get timer value. */
	uint64_t GetTimerValue() const { return m_value ? GetTickCount64() - m_value : 0; }

	/** Get timer value (in ticks). */
	operator uint64_t() const { return m_value ? GetTickCount64() - m_value : 0; }

	/** Get elapsed time in seconds. */
	uint64_t GetTimerSeconds() const { return m_value ? (GetTickCount64() - m_value) / 1000 : 0; }

	/** Get start time of the timer. */
	uint64_t GetTimerStart() const { return m_value; }

#ifndef AMULE_DAEMON
	virtual wxString GetDisplayString() const;
#endif

	virtual bool IsVisible() const { return (m_flags & stHideIfZero) ? m_value != 0 : true; }

protected:
	virtual void AddECValues(CECTag *tag) const;

	//! Tick count value when timer was started.
	uint64_t m_value;
};

/**
 * Tree item for shared files average size. The average is dividend / divisor, if the
 * divisor is non-zero.
 */
class CStatTreeItemAverage : public CStatTreeItemBase
{
public:
	/**
	 * @param dividend What to divide.
	 * @param divisor Divide by what.
	 */
	CStatTreeItemAverage(const wxString &label,
		const CStatTreeItemCounter *dividend,
		const CStatTreeItemCounter *divisor,
		enum EDisplayMode displaymode)
	: CStatTreeItemBase(label, stNone)
	, m_dividend(dividend)
	, m_divisor(divisor)
	, m_displaymode(displaymode)
	{
	}

#ifndef AMULE_DAEMON
	virtual wxString GetDisplayString() const;
#endif

	virtual bool IsVisible() const { return (*m_divisor) != 0; }

protected:
	virtual void AddECValues(CECTag *tag) const;

	//! What to divide.
	const CStatTreeItemCounter *m_dividend;

	//! Divide by what.
	const CStatTreeItemCounter *m_divisor;

	//! Display mode.
	enum EDisplayMode m_displaymode;
};

/** Tree item for average up/down speed. */
class CStatTreeItemAverageSpeed : public CStatTreeItemBase
{
public:
	/**
	 * @param counter Session up/down counter.
	 * @param timer Session uptime timer.
	 */
	CStatTreeItemAverageSpeed(const wxString &label,
		const CStatTreeItemUlDlCounter *counter,
		const CStatTreeItemTimer *timer)
	: CStatTreeItemBase(label, stNone)
	, m_counter(counter)
	, m_timer(timer)
	{
	}

#ifndef AMULE_DAEMON
	virtual wxString GetDisplayString() const;
#endif

protected:
	virtual void AddECValues(CECTag *tag) const;

	//! Session sent/received bytes counter.
	const CStatTreeItemUlDlCounter *m_counter;

	//! Session uptime.
	const CStatTreeItemTimer *m_timer;
};

/**
 * Tree item displaying the ratio between two counters, as counter1:counter2.
 */
class CStatTreeItemRatio : public CStatTreeItemBase
{
public:
	/**
	 * @param cnt1 First counter to use.
	 * @param cnt2 Second counter to use.
	 */
	CStatTreeItemRatio(const wxString &label,
		const CStatTreeItemCounter *cnt1,
		const CStatTreeItemCounter *cnt2,
		uint64_t (*totalfunc1)() = NULL,
		uint64_t (*totalfunc2)() = NULL)
	: CStatTreeItemBase(label, stNone)
	, m_counter1(cnt1)
	, m_counter2(cnt2)
	, m_totalfunc1(totalfunc1)
	, m_totalfunc2(totalfunc2)
	{
	}

#ifndef AMULE_DAEMON
	virtual wxString GetDisplayString() const;
#endif

protected:
	virtual void AddECValues(CECTag *tag) const;

	//! First counter.
	const CStatTreeItemCounter *m_counter1;

	//! Second counter.
	const CStatTreeItemCounter *m_counter2;

	//! A function for each whose return value is the total (without current) value.
	uint64_t (*m_totalfunc1)();
	uint64_t (*m_totalfunc2)();

private:
	//! Formatted "a : b (c : d)" ratio string. cLocale=true renders C-locale
	//! numbers and an untranslated English "Not available" for the EC/API wire
	//! (locale-independent); the default renders in the GUI locale for display.
	wxString GetString(bool cLocale = false) const;
};

/** Special counter for reconnects. */
class CStatTreeItemReconnects : public CStatTreeItemNativeCounter
{
public:
	CStatTreeItemReconnects(const wxString &label)
	: CStatTreeItemNativeCounter(label, stNone)
	{
	}

#ifndef AMULE_DAEMON
	virtual wxString GetDisplayString() const;
#endif

protected:
	virtual void AddECValues(CECTag *tag) const;
};

/** Special item for Max Connection Limit Reached */
class CStatTreeItemMaxConnLimitReached : public CStatTreeItemBase
{
public:
	CStatTreeItemMaxConnLimitReached(const wxString &label)
	: CStatTreeItemBase(label)
	, m_count(0)
	{
	}

	/** Increase counter and save time. */
	void operator++()
	{
		++m_count;
		m_time.SetToCurrent();
	}

#ifndef AMULE_DAEMON
	/**
	 * Returns a string to be displayed on the GUI: "Never" for m_count == 0, otherwise
	 * the counter value plus the date and time of the event.
	 */
	virtual wxString GetDisplayString() const;
#endif

protected:
	virtual void AddECValues(CECTag *tag) const;

	//! Number of times max conn limit reached.
	uint32_t m_count;

	//! Last time when max conn limit reached.
	wxDateTime m_time;
};

/** Special item for total client count */
class CStatTreeItemTotalClients : public CStatTreeItemBase
{
public:
	/**
	 * @param known Counter that counts known clients.
	 * @param unknown Counter that counts unknown clients.
	 */
	CStatTreeItemTotalClients(
		const wxString &label, const CStatTreeItemCounter *known, const CStatTreeItemCounter *unknown)
	: CStatTreeItemBase(label)
	, m_known(known)
	, m_unknown(unknown)
	{
	}

#ifndef AMULE_DAEMON
	virtual wxString GetDisplayString() const;
#endif

protected:
	virtual void AddECValues(CECTag *tag) const;

	//! Counter counting known clients.
	const CStatTreeItemCounter *m_known;

	//! Counter counting unknown clients.
	const CStatTreeItemCounter *m_unknown;
};

#endif /* !CLIENT_GUI */

#endif /* STATTREE_H */
// File_checked_for_headers
