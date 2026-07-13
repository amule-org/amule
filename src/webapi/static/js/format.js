// Pure value formatters shared by every view. No DOM, no framework —
// just the presentation rules that match aMule Web.

import { t } from "./i18n.js";

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

// "session / total" pair from an object's two counter fields (e.g. xfer,
// requests, accepts). Shared by the Shared table and its detail panel.
export function twin(o, a, b, fmt) {
  return fmt((o && o[a]) || 0) + " / " + fmt((o && o[b]) || 0);
}

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
