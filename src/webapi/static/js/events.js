// Live data layer: seeds domain collections from REST, then keeps them
// fresh from the SSE stream (/api/v0/events). If SSE can't connect it
// transparently falls back to periodic polling so the UI stays live.
//
// A "resource" is a list endpoint that has matching SSE deltas, e.g.
// downloads (download_added/updated/removed). Views register their
// resource once, call ensure(key) to start it, then subscribe to the
// store key of the same name to render.

import { store } from "./store.js";
import { api } from "./api.js";

const POLL_INTERVAL_MS = 4000;
const SSE_FAIL_THRESHOLD = 3;

const resources = new Map();   // key -> spec
const collections = new Map(); // key -> Map(id -> item)
const active = new Set();       // keys currently seeded/streamed
const seedBuffers = new Map();  // key -> array of deltas that arrived mid-seed

let es = null;
let sseFails = 0;
let pollTimer = null;
let statusActive = false;

export const data = {
  // spec: { key, eventPrefix, id, list:()=>Promise<array>, channel? }
  register(spec) {
    if (!resources.has(spec.key)) resources.set(spec.key, spec);
  },

  async ensure(key) {
    if (active.has(key)) return;
    active.add(key);
    openSse();
    attachResourceListeners(resources.get(key));
    await seed(key);
  },

  async ensureStatus() {
    if (!statusActive) {
      statusActive = true;
      openSse();
      await refreshStatus();
    }
  },

  // Force a re-fetch of a resource (used after a mutation when waiting
  // for the SSE echo would feel laggy).
  refresh(key) { return active.has(key) ? seed(key) : Promise.resolve(); },

  isLive() { return es !== null && es.readyState === EventSource.OPEN; },

  // Full teardown: no SSE, no poll timer, no pending publish, nothing
  // seeded. Called when the session dies (an amuleapi restart invalidates
  // the cookie) and on logout. Everything here is module-level and survives
  // the shell unmounting, so without this the poll loop keeps firing one
  // request per active resource every 4 s against a session the server has
  // already rejected — which is exactly what earns the IP a ban. Leaves the
  // registry intact: views re-register once per module load, and ensure()
  // re-seeds them after the next login.
  stop() {
    if (es) { es.close(); es = null; }
    stopPolling();
    for (const timer of publishTimers.values()) clearTimeout(timer);
    publishTimers.clear();
    seedBuffers.clear();
    collections.clear();
    listenersAttached.clear();
    // Unpublish the rows too, so after the next login a view spins instead of
    // showing the dead session's list until the new snapshot lands.
    for (const k of active) store.set(k, undefined);
    active.clear();
    statusActive = false;
    sseFails = 0;
    store.set("live", false);
  },
};

async function seed(key) {
  const spec = resources.get(key);
  if (!spec) return;
  // Buffer-then-replay (EVENTS.md §Bootstrap): start buffering deltas
  // *before* awaiting the snapshot, so a delta (esp. a _removed) that lands
  // during the fetch isn't clobbered by the snapshot replacing the map.
  const buf = [];
  seedBuffers.set(key, buf);
  try {
    const arr = (await spec.list()) || [];
    // stop() may have landed while the snapshot was in flight; don't
    // resurrect a collection the teardown just dropped.
    if (!active.has(key)) return;
    const m = new Map();
    for (const it of arr) m.set(String(it[spec.id]), it);
    // Drain buffered deltas over the fresh snapshot, in arrival order.
    for (const { op, payload } of buf) {
      const idv = String(payload[spec.id]);
      if (op === "del") m.delete(idv); else m.set(idv, payload);
    }
    collections.set(key, m);
    publish(key);
  } catch (e) {
    console.error("seed " + key + " failed", e);
    // An unset key means "still loading", so a first seed that never lands
    // would spin forever: fall back to empty (the poll loop retries). Guarded
    // on unset so a failed refresh() keeps its rows, and on active so a 401 —
    // which runs stop() before rejecting here — stays torn down.
    if (active.has(key) && store.get(key) === undefined) store.set(key, []);
  } finally {
    seedBuffers.delete(key); // flip to direct-apply
  }
}

function publish(key) {
  const m = collections.get(key) || new Map();
  store.set(key, Array.from(m.values()));
}

// Coalesce bursty SSE deltas. A busy queue (many downloads/shared/clients
// updating per second) would otherwise re-render the whole table on every
// single delta and lock the main thread. Trailing throttle: at most one
// publish per window per key. Seeds/refreshes still publish immediately, so
// initial load and post-mutation refreshes stay snappy.
const PUBLISH_THROTTLE_MS = 500;
const publishTimers = new Map();
function publishThrottled(key) {
  if (publishTimers.has(key)) return;
  publishTimers.set(key, setTimeout(() => { publishTimers.delete(key); publish(key); }, PUBLISH_THROTTLE_MS));
}

async function refreshStatus() {
  try {
    const s = await api.get("status");
    if (statusActive) store.set("status", s);
  } catch (e) { /* keep last known status */ }
}

// --- SSE ---------------------------------------------------------------
function openSse() {
  if (es) return;
  es = new EventSource(window.location.pathname.replace(/\/?$/, "/") + "api/v0/events");

  es.addEventListener("open", () => {
    sseFails = 0;
    stopPolling();
    store.set("live", true);
  });

  es.addEventListener("error", () => {
    // EventSource auto-reconnects; only flip to polling after repeated
    // failures so a single blip doesn't thrash.
    sseFails++;
    store.set("live", false);
    // ...except when the browser gave up for good. A response it refuses to
    // stream — a 401 once amuleapi has been restarted, a wrong content type —
    // fails the connection permanently (readyState CLOSED, no retry), so
    // waiting out a retry budget that will never be spent would leave the UI
    // frozen on stale data. Fall back now; polling's first request is what
    // surfaces the dead session.
    if (es && es.readyState === EventSource.CLOSED) sseFails = SSE_FAIL_THRESHOLD;
    if (sseFails >= SSE_FAIL_THRESHOLD) startPolling();
  });

  es.addEventListener("status_changed", (ev) => {
    try { store.set("status", JSON.parse(ev.data)); } catch (_) {}
  });

  es.addEventListener("log_appended", (ev) => {
    try { store.set("log:appended", JSON.parse(ev.data)); } catch (_) {}
  });

  // Search has its own channel but doesn't fit the added/updated/removed
  // resource model (no _removed; each search is a fresh result space), so
  // it's surfaced via store keys the search view consumes directly.
  // search_result_added is byte-for-byte a /search/{id}/results[] entry (keyed
  // by hash, nested sources {total, complete}); search_progress carries
  // {state, percent, results, kind} and its terminal frame (state:
  // "finished") is the completion signal. Inert unless a search is active.
  //
  // search_result_updated carries the identical payload for a row that
  // changed after it arrived -- a hit you started downloading, or a Kad note
  // that landed. Both land on the same store key because the view keys by
  // hash and overwrites, which is exactly the upsert the two events describe
  // between them; they stay distinct on the wire so a consumer that only
  // wants additions is not silently given updates too.
  const onSearchResult = (ev) => {
    try { store.set("search:result", JSON.parse(ev.data)); } catch (_) {}
  };
  es.addEventListener("search_result_added", onSearchResult);
  es.addEventListener("search_result_updated", onSearchResult);
  es.addEventListener("search_progress", (ev) => {
    try { store.set("search:progress", JSON.parse(ev.data)); } catch (_) {}
  });

  // search_closed: the slot is gone (freed by a DELETE from any client, an
  // EC reset, or the slot cap). A view bound to that id has nothing left to
  // read, so surface it and let the view decide.
  es.addEventListener("search_closed", (ev) => {
    try { store.set("search:closed", JSON.parse(ev.data)); } catch (_) {}
  });

  // Comments ride their own event (EVENTS.md §comments_updated) instead of
  // download_updated, so the per-tick download frame stays lean. The payload is
  // the GET downloads/{hash}/comments body plus `hash` — the detail view
  // applies it directly rather than re-fetching.
  es.addEventListener("comments_updated", (ev) => {
    try { store.set("comments:updated", JSON.parse(ev.data)); } catch (_) {}
  });

  es.addEventListener("resync", () => {
    if (statusActive) refreshStatus();
    for (const k of active) seed(k);
  });

  for (const spec of resources.values()) attachResourceListeners(spec);
}

const listenersAttached = new Set();
function attachResourceListeners(spec) {
  if (!es || !spec || listenersAttached.has(spec.key)) return;
  listenersAttached.add(spec.key);
  es.addEventListener(spec.eventPrefix + "_added", (ev) => applyDelta(spec, "set", ev));
  es.addEventListener(spec.eventPrefix + "_updated", (ev) => applyDelta(spec, "set", ev));
  es.addEventListener(spec.eventPrefix + "_removed", (ev) => applyDelta(spec, "del", ev));
}

function applyDelta(spec, op, ev) {
  let payload;
  try { payload = JSON.parse(ev.data); } catch (_) { return; }
  // Still seeding this key — buffer instead of applying so seed() can drain
  // these over the snapshot once it lands.
  const buf = seedBuffers.get(spec.key);
  if (buf) { buf.push({ op, payload }); return; }
  let m = collections.get(spec.key);
  if (!m) { m = new Map(); collections.set(spec.key, m); }
  const idv = String(payload[spec.id]);
  if (op === "del") m.delete(idv); else m.set(idv, payload);
  publishThrottled(spec.key);
}

// --- polling fallback --------------------------------------------------
function startPolling() {
  if (pollTimer) return;
  store.set("polling", true);
  let ticking = false;
  const tick = async () => {
    if (ticking) return; // a slow link shouldn't stack ticks on top of each other
    ticking = true;
    try {
      // Status goes first and is awaited: it is the cheapest probe we have,
      // so a dead session latches the api gate (and tears this loop down)
      // before the same tick's resource fetches go out — one rejected
      // request per restart instead of one per active resource.
      if (statusActive) await refreshStatus();
      for (const k of active) seed(k);
    } finally {
      ticking = false;
    }
  };
  tick();
  pollTimer = setInterval(tick, POLL_INTERVAL_MS);
}

function stopPolling() {
  if (pollTimer) { clearInterval(pollTimer); pollTimer = null; }
  store.set("polling", false);
}
