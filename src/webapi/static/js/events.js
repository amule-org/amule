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
  try { store.set("status", await api.get("status")); }
  catch (e) { /* keep last known status */ }
}

// --- SSE ---------------------------------------------------------------
function openSse() {
  if (es) return;
  es = new EventSource("/api/v0/events");

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
  // search_result_added is byte-for-byte a /search/results[] entry (keyed by
  // hash, nested sources {total, complete}); search_progress carries
  // {state, percent, results, kind} and its terminal frame (state:
  // "finished") is the completion signal. Inert unless a search is active.
  es.addEventListener("search_result_added", (ev) => {
    try { store.set("search:result", JSON.parse(ev.data)); } catch (_) {}
  });
  es.addEventListener("search_progress", (ev) => {
    try { store.set("search:progress", JSON.parse(ev.data)); } catch (_) {}
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
  const tick = () => {
    if (statusActive) refreshStatus();
    for (const k of active) seed(k);
  };
  tick();
  pollTimer = setInterval(tick, POLL_INTERVAL_MS);
}

function stopPolling() {
  if (pollTimer) { clearInterval(pollTimer); pollTimer = null; }
  store.set("polling", false);
}
