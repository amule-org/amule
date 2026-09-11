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
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//

#ifndef SHAREDDIRWATCHER_H
#define SHAREDDIRWATCHER_H

#include <wx/event.h>
#include <wx/timer.h>
#include <wx/fswatcher.h>

#include <set>
#include <unordered_map>
#include <vector>

#include "Types.h"

class CSharedFileList;
class CPath;

// Watches every directory in CPreferences::shareddir_list for file/dir creation,
// deletion, rename and modification, and triggers a debounced
// CSharedFileList::Reload() so newly-added files become shared without a manual
// "Reload shared files" click.
//
// New subdirectories created under any watched path are auto-added to
// shareddir_list so the watcher keeps following them, mirroring what the
// "recursive share" button already does. No per-entry recursive flag is
// persisted, because shareddir.dat enumerates each subdirectory individually.
//
// Backends come from wxFileSystemWatcher: inotify on Linux, FSEvents on macOS,
// ReadDirectoryChangesW on Windows, kqueue on BSD. AddTree() handles the initial
// recursive setup; new subdirs are re-Added individually as they are discovered.
class CSharedDirWatcher : public wxEvtHandler
{
public:
	explicit CSharedDirWatcher(CSharedFileList *parent);
	~CSharedDirWatcher();

	// Start watching every path in shareddir_list. Safe to call when
	// already enabled — no-op in that case.
	void Enable();

	// Stop watching. The watcher object itself is freed from the event
	// queue rather than here -- see Disable(). Safe to call when already
	// disabled.
	void Disable();

	bool IsEnabled() const { return m_watcher != NULL; }

	// Called from CSharedFileList::Reload() after the list has been rebuilt.
	// Re-walks shareddir_list and updates the watcher's path set, so removed dirs
	// stop firing events and newly-added ones start.
	void Refresh();

	// Event-type flags accumulated for one pending path between
	// fs-watcher delivery and debounce-flush. Stored as ints so we
	// don't have to expose wxFSW_EVENT_* in the header to includers.
	struct PendingPathEvents
	{
		int flags;          // bitmask of wxFSW_EVENT_CREATE/DELETE/MODIFY
		wxString renamedTo; // populated for RENAME; empty otherwise
	};

private:
	void OnFileSystemEvent(wxFileSystemWatcherEvent &event);
	void OnDebounceTimer(wxTimerEvent &event);
#ifdef __APPLE__
	// macOS amuled (wxAppConsole) does not spin the main thread's CFRunLoop, so
	// FSEvents callbacks scheduled by wx never deliver; a periodic non-blocking
	// drain fixes that without a dedicated thread. No-op under aMule.app.
	void OnMacRunLoopPump(wxTimerEvent &event);
#endif

	// Walk shareddir_list and Add() every path. Errors per-path are
	// logged but do not abort the rest (so a single Linux
	// max_user_watches refusal doesn't blank the whole watcher).
	void RegisterAllPaths();

	// Coalesce a burst of FS events into per-path deltas applied on
	// the debounce-timer flush. Resets the 5-second timer on every
	// new event; processing runs when the timer finally fires.
	void ScheduleProcessing();

	// Walk m_pendingEvents and apply each one to CSharedFileList. Skipped when a
	// resync is owed from a wxFSW_EVENT_WARNING / _ERROR since the last flush: the
	// backend has signalled it dropped events, so the incremental view cannot be
	// trusted and the bulk Reload() takes over.
	void FlushPendingEvents();

	// Append a newly-created directory to shareddir_list (if not
	// already there) and start watching it. Used for Option A's
	// "auto-share new subdirs of watched parents" behaviour.
	void RegisterNewSubdirectory(const wxString &path);

	// Tear down a renamed-away / deleted shared dir: detach its subtree, drop it
	// from the runtime shared set + its watch. Never touches the user's
	// explicit/recursive config. Returns true if it removed a set entry.
	bool HandleDirRemoved(const wxString &path);

	// Is `path` in the runtime shared set? Distinguishes a dir event from a
	// file event once the path is gone from disk.
	bool IsInSharedSet(const wxString &path) const;

	// Closes the inotify/kqueue race window inside RegisterNewSubdirectory: between
	// the kernel mkdir and our wxFileSystemWatcher::Add(), anything created inside
	// the new directory fires on a watch that does not exist yet and is silently
	// dropped. Walks `parent` once the watch is in place and feeds existing entries
	// through NotifyPathAdded and RegisterNewSubdirectory, so the catch-up matches
	// what the live event path would have done. Idempotent.
	void ScanNewSubdirRace(const CPath &parent);

	// Recursively walk each path in shareddir_list and add any subdir not already
	// listed. The "cold" twin of RegisterNewSubdirectory: the watcher only fires
	// CREATE events post-Enable(), so without this a /Music share that grew three
	// albums while aMule was offline would never see them. Batches in memory and
	// writes shareddir.dat once at the end.
	void ColdDiscoverSubdirs();

	// Recursive walker used by ColdDiscoverSubdirs. Visits every subdirectory of
	// `root`, inserting each one not already in `known` into `known` and `out`.
	// Always recurses, even through known subdirs, so a tree whose top layer is in
	// shareddir.dat but whose deeper layers are not is still fully covered.
	void WalkForUnknownSubdirs(const CPath &root, std::set<wxString> &known, std::vector<CPath> &out);

	// Destroy the watchers Disable() detached. Queued with CallAfter() so
	// the delete lands between dispatches; also called by the destructor
	// for whatever the queue never reached.
	void ReapPendingWatchers();

	// Stop the debounce (and, on macOS, the run-loop pump) timer. Shared
	// by Disable() and the destructor.
	void StopTimers();

	CSharedFileList *m_parent;
	wxFileSystemWatcher *m_watcher;
	//! Watchers detached by Disable() and not yet destroyed. ~wxFileSystemWatcher
	//! frees the wxFDIOEventLoopSourceHandler wx registered for its inotify fd, and
	//! wxEpollDispatcher::Dispatch walks a snapshot of the ready events without
	//! re-checking any handler -- so freeing one from inside an event handler makes
	//! that loop call a virtual on freed memory. Destruction is therefore deferred
	//! to the pending-event queue, which the loop drains before it dispatches.
	std::vector<wxFileSystemWatcher *> m_pendingDelete;
	wxTimer m_debounceTimer;

	// Per-path accumulator. Keyed on the raw filesystem path. Events
	// coalesce per-path: a CREATE followed by N MODIFYs followed by
	// CLOSE_WRITE on the same file becomes a single dispatch on the
	// debounce flush. RENAME stores the destination in `renamedTo`.
	std::unordered_map<wxString, PendingPathEvents> m_pendingEvents;

	// Why a full-reload resync is owed; cleared once the fallback fires.
	//
	// ResyncDroppedEvents means the backend reported overflow or drop (inotify
	// queue overflow, kqueue race, ReadDirectoryChangesW buffer exhaust), so
	// FlushPendingEvents() falls back to the bulk Reload() and logs a real fault.
	//
	// ResyncColdDiscovery means subdirectories simply appeared while aMule was not
	// running. Nothing failed. Both reasons used to share one bool and so one
	// message, which reported this routine case as a watcher failure, in red.
	//
	// It is hard to provoke: ReloadSharedFolders expands recursive roots into the
	// shared-dir union and the startup Reload runs before the watcher is enabled,
	// so ColdDiscoverSubdirs normally finds nothing. What remains is the narrow
	// window where a directory appears between that Reload and Enable().
	enum ResyncReason
	{
		ResyncNone,
		//! Backend overflow/error: per-path deltas untrustworthy.
		ResyncDroppedEvents,
		//! New subdirs found at startup; nothing failed.
		ResyncColdDiscovery,
	};
	ResyncReason m_resyncReason = ResyncNone;
	//! Subdirectory count for the ResyncColdDiscovery message.
	unsigned m_coldDiscoveredDirs = 0;
#ifdef __APPLE__
	wxTimer m_macPumpTimer;
#endif

	DECLARE_EVENT_TABLE()
};

#endif // SHAREDDIRWATCHER_H
