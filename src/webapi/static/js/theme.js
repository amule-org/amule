// Theme control: "system" (follow the OS via prefers-color-scheme), "light"
// or "dark". The choice is persisted in localStorage and applied by setting
// (or clearing) data-theme on <html>; app.css resolves the rest:
//   - no data-theme            -> :root defaults + @media(prefers-color-scheme)
//   - data-theme="light|dark"  -> explicit override that beats the media query
//
// A tiny inline script in index.html applies the saved choice before first
// paint to avoid a flash; this module keeps it in sync at runtime.

const KEY = "amule.theme";
const ORDER = ["system", "light", "dark"];

function read() {
  try {
    const v = localStorage.getItem(KEY);
    return ORDER.includes(v) ? v : "system";
  } catch (_) {
    return "system";
  }
}

export function applyTheme(pref) {
  const root = document.documentElement;
  if (pref === "light" || pref === "dark") root.dataset.theme = pref;
  else delete root.dataset.theme; // "system"
}

export function getTheme() {
  return read();
}

export function setTheme(pref) {
  const next = ORDER.includes(pref) ? pref : "system";
  try { localStorage.setItem(KEY, next); } catch (_) {}
  applyTheme(next);
  return next;
}

// system -> light -> dark -> system
export function cycleTheme() {
  const i = ORDER.indexOf(read());
  return setTheme(ORDER[(i + 1) % ORDER.length]);
}

// Apply the saved choice as soon as the module loads.
applyTheme(read());
