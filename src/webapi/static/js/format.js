// Pure value formatters shared by every view. No DOM, no framework —
// just the presentation rules that match aMule Web.

import { t } from "./i18n.js";

// IEC, because the scaling below is by 1024 -- same reasoning as
// CastItoXBytes on the GUI side.
const UNITS = ["B", "KiB", "MiB", "GiB", "TiB", "PiB"];

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

// Pick ONE byte unit for a whole axis from its max value, so chart ticks can be
// bare numbers with the unit shown once instead of repeating "KB/s" per tick.
// Returns the divisor to scale raw values by and the unit label to display.
export function bytesAxis(max, perSec) {
  let i = 0, n = Number(max) || 0;
  while (n >= 1024 && i < UNITS.length - 1) { n /= 1024; i++; }
  return { div: Math.pow(1024, i), unit: UNITS[i] + (perSec ? "/s" : "") };
}

export function formatPercent(p) {
  p = Number(p) || 0;
  return p.toFixed(p >= 100 || p === 0 ? 0 : 1) + "%";
}

export function formatInt(n) {
  return (Number(n) || 0).toLocaleString();
}

// "session / total" pair from a row's two sibling counter fields (e.g.
// uploaded_bytes_session + uploaded_bytes_total). Shared by the Shared table
// and its detail panel. The counters used to live in `xfer` / `requests` /
// `accepts` wrapper objects; they were flattened, so this takes the row and
// two key names rather than a sub-object.
export function twin(row, a, b, fmt) {
  return fmt((row && row[a]) || 0) + " / " + fmt((row && row[b]) || 0);
}

// Free disk space for the two directories /status reports, as one string. Temp
// and Incoming are the same filesystem in the default layout, so a single bare
// figure is the common case; when they differ each is labelled. null (the daemon
// has no figure) is left out rather than shown as 0, which would read as a full
// disk -- with both null the caller gets "" and drops the entry.
export function formatFreeSpace(temp, incoming) {
  if (temp == null && incoming == null) return "";
  const T = t("common_free_space_temp"), I = t("common_free_space_incoming");
  if (temp == null) return I + " " + formatBytes(incoming);
  if (incoming == null) return T + " " + formatBytes(temp);
  // Compared as rendered, not as bytes: amuled samples the two directories
  // separately, so one filesystem still drifts a block between publishes and an
  // exact == would flip between one and two figures every few seconds.
  const a = formatBytes(temp), b = formatBytes(incoming);
  return a === b ? a : T + " " + a + " / " + I + " " + b;
}

// Unix seconds -> locale date+time; 0 means unknown.
export const formatTimestamp = (s) => s ? new Date(s * 1000).toLocaleString() : "—";

// Seconds -> human duration, mirroring CastSecondsToHM (src/OtherFunctions.cpp).
export function formatDuration(s) {
  s = Math.floor(Number(s) || 0);
  const pad = (n) => String(n).padStart(2, "0");
  if (s < 60) return pad(s) + " " + t("stats_secs");
  if (s < 3600) return Math.floor(s / 60) + ":" + pad(s % 60) + " " + t("stats_mins");
  if (s < 86400) return Math.floor(s / 3600) + ":" + pad(Math.floor((s % 3600) / 60)) + " " + t("stats_hours");
  return Math.floor(s / 86400) + " " + t("stats_days") + " " +
    pad(Math.floor((s % 86400) / 3600)) + ":" + pad(Math.floor((s % 3600) / 60)) + " " + t("stats_hours");
}
