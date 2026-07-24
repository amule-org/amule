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
