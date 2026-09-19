// App state, two flavours:
//   - `store`: a minimal in-memory pub/sub container (lost on reload). Keys are
//     domain names ("downloads", "status", …); views subscribe to a key and
//     re-render when it changes.
//   - loadPref/savePref: persistent prefs backed by localStorage, JSON
//     round-trip under an "amule."-prefixed key (matching theme.js / i18n.js).
//     Every access is guarded so a disabled / full / private-mode localStorage
//     degrades to the caller's fallback instead of throwing.
//     ponytail: JSON get/set only; no TTL/versioning until something needs it.

const state = new Map();
const subs = new Map(); // key -> Set<fn>

export const store = {
  get(key) { return state.get(key); },

  set(key, value) {
    state.set(key, value);
    const set = subs.get(key);
    if (set) for (const fn of set) { try { fn(value); } catch (e) { console.error(e); } }
  },

  // Subscribe to a key. Returns an unsubscribe function. If the key
  // already has a value, the subscriber is called immediately with it.
  subscribe(key, fn) {
    if (!subs.has(key)) subs.set(key, new Set());
    subs.get(key).add(fn);
    if (state.has(key)) { try { fn(state.get(key)); } catch (e) { console.error(e); } }
    return () => { const s = subs.get(key); if (s) s.delete(fn); };
  },
};

export function loadPref(key, fallback) {
  try {
    const v = localStorage.getItem("amule." + key);
    return v == null ? fallback : JSON.parse(v);
  } catch (_) {
    return fallback;
  }
}

export function savePref(key, val) {
  try {
    localStorage.setItem("amule." + key, JSON.stringify(val));
  } catch (_) {}
}

// Wipe every persisted WebUI pref: theme, lang, table layouts, panel heights --
// all live under the "amule." prefix.
export function clearPrefs() {
  try {
    for (const k of Object.keys(localStorage))
      if (k.startsWith("amule.")) localStorage.removeItem(k);
  } catch (_) {}
}

// --- Graph time range (Statistics + Networks/Kad) ------------------------
// The persisted sampling interval (seconds) sent as
// /stats/graphs?interval_seconds=N; at 300 samples it sets how far back the
// window reaches. interval -> window: 5min / 1h / 6h / 24h. Each is well above
// the record spacing at its window's far edge, so amuled never repeats records
// (see docs/api/REFERENCE.md, GET /stats/graphs/{graph}).
export const GRAPH_RANGES = [
  { interval: 1, labelKey: "prefs_graph_range_5m" },
  { interval: 12, labelKey: "prefs_graph_range_1h" },
  { interval: 72, labelKey: "prefs_graph_range_6h" },
  { interval: 288, labelKey: "prefs_graph_range_24h" },
];

const DEFAULT_GRAPH_INTERVAL = 12; // 1 hour

// Validate against the presets so a stale/edited value can't reach amuled.
export function loadGraphInterval() {
  const v = loadPref("stats.graphRange", DEFAULT_GRAPH_INTERVAL);
  return GRAPH_RANGES.some((r) => r.interval === v) ? v : DEFAULT_GRAPH_INTERVAL;
}

export function saveGraphInterval(interval) {
  savePref("stats.graphRange", interval);
}
