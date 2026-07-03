// Pure value formatters shared by every view. No DOM, no framework —
// just the presentation rules that match aMule Web.

const UNITS = ["B", "KB", "MB", "GB", "TB", "PB"];

export function formatBytes(n) {
  n = Number(n) || 0;
  let i = 0;
  while (n >= 1024 && i < UNITS.length - 1) { n /= 1024; i++; }
  const digits = (i === 0 || n >= 100) ? 0 : (n >= 10 ? 1 : 2);
  return n.toFixed(digits) + " " + UNITS[i];
}

export function formatSpeed(bps) {
  bps = Number(bps) || 0;
  if (bps <= 0) return "—";
  return formatBytes(bps) + "/s";
}

export function formatPercent(p) {
  p = Number(p) || 0;
  return p.toFixed(p >= 100 || p === 0 ? 0 : 1) + "%";
}

export function formatInt(n) {
  return (Number(n) || 0).toLocaleString();
}
