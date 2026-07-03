// Minimal pub/sub state container. Keys are domain names ("downloads",
// "status", …); values are whatever the data layer or a view puts there.
// Views subscribe to a key and re-render when it changes.

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
