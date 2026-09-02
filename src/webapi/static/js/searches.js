// Multi-search registry: one entry per open search tab, kept OUTSIDE the view
// so tabs survive navigating to another section (the view unmounts, this
// module does not).
//
// The three SSE search events each ride one store key; this module is their
// only subscriber and routes them by search_id, so per-id keys would buy
// nothing. It publishes at two granularities, so a burst of results on a
// BACKGROUND tab never re-renders the visible table:
//   store "searches"    -> { tabs: [light tab descriptors], activeId }
//   store "search:<id>" -> that search's results array, active tab only.

import { api } from "./api.js";
import { data } from "./events.js";
import { store } from "./store.js";
import { toast } from "./components.js";
import { t, terr } from "./i18n.js";

// Trailing throttles, separate because the strip is cheap and the table is not.
const TABS_SYNC_MS = 1000;
const RESULTS_SYNC_MS = 1000;
// Fallback polling, only while SSE is down. A finished tab is never polled --
// nothing about it changes except on the explicit re-reads below.
const POLL_ACTIVE_MS = 1500;
const POLL_BG_EVERY = 4; // every 4th tick => ~6 s
// A frame for an unknown id means another client started a search; re-listing
// costs an EC round trip, so debounce it.
const ADOPT_DEBOUNCE_MS = 3000;

const tabs = new Map(); // search_id -> tab
let activeId = 0;
let started = false;
let pollTimer = null;
let tabsTimer = 0;
let resultsTimer = 0;
let lastAdopt = 0;
let offs = [];
let lastResult = null, lastProgress = null, lastClosed = null;

function newTab({ id, query = "", label = "", kind = "global", state = "running", startedAt = 0,
                 percent = 0, count = 0 }) {
  return {
    id, query, label: label || query, kind, state, percent, startedAt,
    results: new Map(), count, fetching: false,
    // Per-tab UI state: switching tabs and coming back must restore it, so it
    // cannot live in the view's component state.
    ui: { selection: new Set(), filter: "", filterHave: "all", cat: 0, rowCat: {}, rowEcid: {} },
  };
}

// GET /search arrives id-ascending, and id order is NOT recency: a Kad id
// carries SEARCH_ID_KAD_MASK, so it always sorts above an ed2k one. Rank by
// started_at instead; an entry without one (another client's, or restored from
// the daemon's ring) is unknown rather than oldest, so it ranks last.
const ord = (s) => (s.started_at ? s.started_at : Number.MAX_SAFE_INTEGER);
const nowSec = () => Math.floor(Date.now() / 1000);

function tabList() {
  return Array.from(tabs.values()).map((x) => ({
    id: x.id, query: x.query, label: x.label, kind: x.kind, state: x.state,
    // percent is 0 on an adopted tab until it is refreshed: GET /search does
    // not carry one, only GET /search/{id}/results does. Harmless, because the
    // view keys its progress text off `state` and only shows a percent while
    // running -- and the tab that is running and visible has been through
    // refresh() via setActive().
    //
    // count is the larger of the daemon's reported total and what this tab
    // has actually pulled: a tab whose results are loaded knows better than
    // the listing snapshot, and an unopened one has only the listing.
    percent: x.percent, count: Math.max(x.count || 0, x.results.size),
    moreExhausted: !!x.moreExhausted,
  }));
}

function publishTabs() {
  if (tabsTimer) { clearTimeout(tabsTimer); tabsTimer = 0; }
  store.set("searches", { tabs: tabList(), activeId });
}
function publishTabsSoon() {
  if (!tabsTimer) tabsTimer = setTimeout(() => { tabsTimer = 0; publishTabs(); }, TABS_SYNC_MS);
}
function publishResults(id) {
  // Bail BEFORE touching the timer: a pending publish always belongs to the
  // active tab, so clearing it while publishing a background one would drop
  // the coalesced update it was holding.
  if (id !== activeId) return;
  if (resultsTimer) { clearTimeout(resultsTimer); resultsTimer = 0; }
  const tab = tabs.get(id);
  store.set("search:" + id, tab ? Array.from(tab.results.values()) : []);
}
function publishResultsSoon(id) {
  if (id !== activeId || resultsTimer) return;
  resultsTimer = setTimeout(() => { resultsTimer = 0; publishResults(id); }, RESULTS_SYNC_MS);
}

// --- reads ---------------------------------------------------------------

// Authoritative re-read: seeds a tab's results on activation and reconciles
// against anything the stream did not push. search_result_updated now pushes a
// held hit's status / already_downloaded / comments / rating live, but its
// source counts and alternate names stay add-only on the stream, so this is
// still how those refresh. amuleapi coalesces it behind a ~1 s TTL, so calling
// it per tab activation is cheap.
async function refresh(id) {
  const tab = tabs.get(id);
  if (!tab || tab.fetching) return;
  tab.fetching = true;
  try {
    const r = await api.list("search/" + id + "/results");
    const cur = tabs.get(id);
    if (!cur) return;
    cur.results = new Map((r.results || []).map((x) => [x.hash, x]));
    cur.count = cur.results.size;
    if (r.query && !cur.query) cur.query = r.query;
    if (r.query && !cur.label) cur.label = r.query;
    const pr = r.progress || {};
    if (pr.state) cur.state = pr.state;
    if (pr.type) cur.kind = pr.type;
    cur.percent = pr.percent || 0;
    publishTabs();
    publishResults(id);
  } catch (e) {
    // Gone from the daemon (freed elsewhere, or aged out): search_closed the
    // hard way.
    if (e && e.status === 404) drop(id, true);
  } finally {
    const cur = tabs.get(id);
    if (cur) cur.fetching = false;
  }
}

// Open a tab for every search the daemon holds. Results are fetched lazily on
// first activation: adopting 20 searches must not fire 20 requests.
async function adopt() {
  lastAdopt = Date.now();
  let list;
  try { list = (await api.list("search")).searches || []; } catch (_) { return; }
  list = list.slice().sort((a, b) => ord(a) - ord(b));
  let added = false;
  for (const s of list) {
    const known = tabs.get(s.search_id);
    if (known) {
      if (!known.query && s.query) { known.query = s.query; known.label = known.label || s.query; }
      // Keep an unopened tab's badge current. Only while it holds no results
      // of its own -- once fetched, its own map is the better number and the
      // listing snapshot must not walk it back.
      if (typeof s.result_count === "number" && !known.results.size &&
          s.result_count !== known.count) {
        known.count = s.result_count;
        added = true;
      }
      continue;
    }
    // result_count is what makes an unopened tab show a real badge instead of
    // 0 after a reload; it is omitted by a daemon that does not report counts,
    // in which case the badge stays 0 until the tab is opened and fetched.
    tabs.set(s.search_id, newTab({
      id: s.search_id, query: s.query || "", kind: s.type || "global",
      state: s.state || "finished", startedAt: s.started_at || 0,
      count: typeof s.result_count === "number" ? s.result_count : 0,
    }));
    added = true;
  }
  if (!tabs.has(activeId)) {
    const running = list.filter((s) => s.state === "running");
    const pick = (running.length ? running : list).slice(-1)[0];
    if (pick) { setActive(pick.search_id); return; }
    activeId = 0;
  }
  if (added) publishTabs();
}

// Debounced adopt, so the first mount's own adopt() is not doubled.
function nudgeAdopt() {
  if (!started || Date.now() - lastAdopt < ADOPT_DEBOUNCE_MS) return;
  adopt();
}

function setActive(id) {
  activeId = Number(id) || 0;
  publishTabs();
  if (!tabs.get(activeId)) return;
  publishResults(activeId); // whatever is already held, instantly
  refresh(activeId);        // then reconcile the fields the stream does not push
}

function drop(id, notify) {
  const tab = tabs.get(id);
  if (!tab) return;
  // The tab to the right, else the one to the left -- Map order is strip order.
  const ids = Array.from(tabs.keys());
  const next = ids[ids.indexOf(id) + 1] || ids[ids.indexOf(id) - 1];
  tabs.delete(id);
  store.set("search:" + id, []);
  if (activeId === id) {
    activeId = 0;
    if (next) setActive(next);
    if (notify) toast(t("search_toast_freed", { query: tab.label || tab.query }), "info");
  }
  publishTabs();
}

// --- SSE routing ---------------------------------------------------------

function onResult(p) {
  if (!p || p === lastResult) return;
  lastResult = p;
  const tab = tabs.get(p.search_id);
  if (!tab) { nudgeAdopt(); return; }
  tab.results.set(p.hash, p);
  publishTabsSoon();
  publishResultsSoon(p.search_id);
}

function onProgress(p) {
  if (!p || p === lastProgress) return;
  lastProgress = p;
  const tab = tabs.get(p.search_id);
  if (!tab) { nudgeAdopt(); return; }
  const wasRunning = tab.state === "running";
  if (p.state) tab.state = p.state;
  if (p.type) tab.kind = p.type;
  tab.percent = p.percent || 0;
  // The event's count is the backend's own map size and may legitimately run
  // ahead of the upserts seen so far.
  if (typeof p.result_count === "number") tab.count = p.result_count;
  publishTabsSoon();
  // Terminal frame: reconcile once against REST to pick up anything the
  // stream dropped, and to land the final status/alternate_names of every row.
  if (wasRunning && tab.state === "finished") refresh(tab.id);
}

function onClosed(p) {
  if (!p || p === lastClosed) return;
  lastClosed = p;
  drop(p.search_id, true);
}

// --- fallback polling ----------------------------------------------------

function startPolling() {
  if (pollTimer) return;
  let tick = 0;
  pollTimer = setInterval(() => {
    if (data.isLive()) return; // SSE is the normal path; this is the fallback
    tick++;
    const act = tabs.get(activeId);
    if (act && act.state === "running") refresh(activeId);
    if (tick % POLL_BG_EVERY === 0) {
      for (const tab of tabs.values())
        if (tab.id !== activeId && tab.state === "running") refresh(tab.id);
    }
  }, POLL_ACTIVE_MS);
}

// --- public API ----------------------------------------------------------

export const searches = {
  // First mount of the search view (or of anything that opens a tab). Idempotent.
  ensure() {
    if (started) return;
    started = true;
    lastResult = store.get("search:result");
    lastProgress = store.get("search:progress");
    lastClosed = store.get("search:closed");
    offs.push(store.subscribe("search:result", onResult));
    offs.push(store.subscribe("search:progress", onProgress));
    offs.push(store.subscribe("search:closed", onClosed));
    startPolling();
    adopt();
  },

  // Logout / dead session. Everything here is module-level and outlives the
  // shell, so without this a stale tab keeps polling a dead session.
  reset() {
    for (const off of offs) off();
    offs = [];
    if (pollTimer) { clearInterval(pollTimer); pollTimer = null; }
    if (tabsTimer) { clearTimeout(tabsTimer); tabsTimer = 0; }
    if (resultsTimer) { clearTimeout(resultsTimer); resultsTimer = 0; }
    for (const id of tabs.keys()) store.set("search:" + id, []);
    tabs.clear();
    activeId = 0;
    started = false;
    lastResult = lastProgress = lastClosed = null;
    store.set("searches", { tabs: [], activeId: 0 });
  },

  setActive,
  refresh,
  adopt,
  nudgeAdopt,

  // Per-tab UI state. Read at render time; every write republishes the tab
  // list, which is what re-renders the view.
  ui(id) { const tab = tabs.get(id); return tab ? tab.ui : null; },
  setUi(id, patch) {
    const tab = tabs.get(id);
    if (!tab) return;
    Object.assign(tab.ui, patch);
    publishTabs();
  },

  // POST /search -> a new tab, which becomes active. `label` overrides what
  // the strip shows (a related:: query is unreadable in a tab).
  async start(body, label) {
    // The 202 answers with the created search as the same row GET /search
    // lists, so the tab is seeded from what the daemon recorded rather than
    // from what we asked for -- `kind` and `startedAt` in particular, which
    // the request only implies.
    const r = await api.post("search", body);
    const id = r.search_id;
    tabs.set(id, newTab({
      id, query: r.query || body.query || "", label: label || "",
      kind: r.type || body.type || "global",
      state: r.state || "running",
      startedAt: r.started_at || nowSec(),
    }));
    setActive(id);
    return id;
  },

  // "View files": a browse is an ordinary search of kind "browse", so it gets
  // an ordinary tab. Generic, so any future entry point reuses it.
  //
  // Find-or-create, unlike start(): browsing a peer that is already being
  // browsed returns the id already in flight (amule-org/amule#1059), so a
  // second click lands on the tab that is already open. Overwriting it would
  // throw away its results, its badge and its per-tab ui state (selection,
  // filter, per-row category) for a browse that never restarted.
  async browse(ecid, name) {
    const r = await api.post("clients/" + ecid + "/shared_files");
    const id = r.search_id;
    const open = tabs.get(id);
    if (open) {
      if (name) { open.query = name; open.label = name; }
    } else {
      tabs.set(id, newTab({
        id, query: name || r.query || "", label: name || r.query || "",
        kind: r.type || "browse",
        state: r.state || "running",
        startedAt: r.started_at || nowSec(),
      }));
    }
    setActive(id);
    toast(t("search_toast_browse_started"), "info");
    if (location.hash !== "#/search") location.hash = "#/search";
    return id;
  },

  // Related-files search: no endpoint and no opcode, just a magic keyword and
  // an ordinary LOCAL search. A server without `related_search` answers it with
  // nothing, which reads as "no related files" rather than "unsupported", hence
  // the check. ponytail: checked at click time rather than gating the button,
  // so the view never holds the whole server list for one boolean.
  async related(hashes, label) {
    const ed2k = (store.get("status") || {}).ed2k || {};
    try {
      const list = (await api.list("servers")).servers || [];
      const ip = (v) => { const m = String(v || "").match(/\d+\.\d+\.\d+\.\d+/); return m ? m[0] : ""; };
      const cur = list.find((s) => ip(s.address) && ip(s.address) === ip(ed2k.server_ip)
                                   && s.port === ed2k.server_port);
      if (cur && !(cur.tcp_flags && cur.tcp_flags.related_search)) {
        toast(t("search_related_unsupported"), "warn");
        return 0;
      }
    } catch (_) { /* can't tell -- let the search go rather than block it */ }
    // Upper-case like the desktop (CMD4Hash::Encode): the ed2k server parses
    // this keyword, not us, so don't gamble on it folding case.
    const keyword = hashes.map((h) => String(h).toUpperCase()).join("::");
    return searches.start({ query: "related::" + keyword, type: "local" }, label);
  },

  async stop(id) {
    try { await api.post("search/" + id + "/stop"); } catch (e) { toast(terr(e), "error"); }
    refresh(id);
  },

  // Kad "More": re-ask already-queried nodes for a wider frontier. amuleapi
  // rejects a non-Kad or finished search with a 400.
  //
  // 409 kad_more_exhausted is the terminal one: the daemon's 4-reask budget is
  // spent, or the search has entered the stopping window Kad opens 20 s before
  // it ends. Nothing un-spends it, so the tab remembers and the button goes
  // away for this search only -- the flag dies with the tab, and a re-run gets
  // a new search_id and a clean one. A plain 202 promises no reask went out
  // (no peer left to re-ask *yet* also answers 202), which is why only the 409
  // disables anything.
  async more(id) {
    try {
      await api.post("search/" + id + "/more");
      toast(t("search_toast_more_started"), "success");
    } catch (e) {
      const tab = tabs.get(id);
      if (tab && e && e.code === "kad_more_exhausted") { tab.moreExhausted = true; publishTabs(); }
      toast(terr(e), "error");
    }
  },

  // Stop AND free. A 404 is success: someone else already freed it.
  async close(id) {
    try { await api.del("search/" + id); }
    catch (e) { if (!e || e.status !== 404) { toast(terr(e), "error"); return; } }
    drop(id, false);
  },

  // The desktop's "Clear Search Results": one DELETE per open search, which is
  // all a bulk route would have been anyway. activeId is cleared up front so
  // each close does not activate (and re-read) a tab about to be closed.
  async closeAll() {
    const ids = Array.from(tabs.keys());
    activeId = 0;
    await Promise.all(ids.map(async (id) => {
      try { await api.del("search/" + id); }
      catch (e) { if (!e || e.status !== 404) { toast(terr(e), "error"); return; } }
      tabs.delete(id);
      store.set("search:" + id, []);
    }));
    // Anything a failed DELETE left behind still needs an active tab.
    const first = tabs.keys().next().value;
    if (first) setActive(first); else publishTabs();
  },
};
