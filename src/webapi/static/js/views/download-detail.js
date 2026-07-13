// Downloads detail panel: the per-file info + pieces graph shown below the
// downloads table when a row is clicked. Data comes from GET downloads/{hash}
// (every list field + the detail-only fields + progress.parts). It re-fetches
// on each live tick of the downloads store so %, ETA, speed and the pieces
// graph stay current while the panel is open (GET is ETag-cached, so unchanged
// frames short-circuit to a 304 + the cached body).

import { api } from "../api.js";
import { html, useState, useEffect, useRef, useStore } from "../dom.js";
import { ProgressBar, Placeholder, toast, Section, statRow, magnetLink, copyText } from "../components.js";
import { formatBytes, formatSpeed, formatDuration, formatInt, formatPercent } from "../format.js";
import { Icon } from "../icons.js";
import { t, tn } from "../i18n.js";

// The pieces bar mirrors the aMule GUI download bar, theme-tuned via CSS vars:
// green (--ok) = have it, blue (--piece-avail-lo -> --piece-avail, faded by
// source count) = available, red (--bad) = missing (nobody has it).
// Sources at which an available part reaches full-intensity blue (aMule's
// gradient saturates around 10 sources: blue = 210 - 22*(sources-1)).
const AVAIL_FULL = 10;

// Parse a CSS colour ("#rgb", "#rrggbb", or "rgb(...)") to [r,g,b].
function toRGB(s) {
  s = (s || "").trim();
  if (s[0] === "#") {
    let h = s.slice(1);
    if (h.length === 3) h = h[0] + h[0] + h[1] + h[1] + h[2] + h[2];
    const n = parseInt(h, 16);
    return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
  }
  const m = s.match(/[\d.]+/g);
  return m ? [+m[0], +m[1], +m[2]] : [0, 0, 0];
}
// Linear blend a->b by f in [0,1], as a canvas fillStyle string.
function mix(a, b, f) {
  return "rgb(" + Math.round(a[0] + (b[0] - a[0]) * f) + "," +
    Math.round(a[1] + (b[1] - a[1]) * f) + "," +
    Math.round(a[2] + (b[2] - a[2]) * f) + ")";
}

export function DownloadDetail({ hash }) {
  const downloads = useStore("downloads") || []; // live tick source (SSE ~500ms)
  const [detail, setDetail] = useState(null);
  const [gone, setGone] = useState(false);

  useEffect(() => {
    if (!hash) { setDetail(null); return; }
    let alive = true;
    api.get("downloads/" + hash)
      .then((d) => { if (alive) { setDetail(d); setGone(false); } })
      .catch(() => { if (alive) setGone(true); });
    return () => { alive = false; };
  }, [hash, downloads]);

  if (!hash) return null;
  if (gone) return html`<div class="detail-panel"><${Placeholder} kind="info">${t("downloads_detail_gone")}<//></div>`;
  if (!detail) return html`<div class="detail-panel"><${Placeholder} kind="info">${t("downloads_detail_loading")}<//></div>`;

  const d = detail;
  const src = d.sources || {};
  const media = d.media;
  const fmtTs = (s) => s ? new Date(s * 1000).toLocaleString() : "—";
  const eta = (d.remaining_time == null || d.remaining_time < 0) ? "—" : formatDuration(d.remaining_time);

  const copy = (text) => copyText(text)
    .then(() => toast(t("downloads_detail_copied"), "success"))
    .catch(() => toast(t("downloads_detail_copy_failed"), "error"));

  return html`
    <div class="detail-panel">
      <div class="detail-head">
        <div class="detail-titlebar">
          <h4 class="detail-name" title=${d.name}>${d.name}</h4>
          <div class="detail-actions">
            <button class="btn btn-sm" type="button" onClick=${() => copy(d.ed2k_link)}>
              <${Icon} name="copy" /> ${t("downloads_detail_copy_ed2k")}
            </button>
            <button class="btn btn-sm" type="button" onClick=${() => copy(magnetLink(d))}>
              <${Icon} name="copy" /> ${t("downloads_detail_copy_magnet")}
            </button>
          </div>
        </div>
        <${ProgressBar} percent=${d.progress && d.progress.percent} />
        <${PiecesBar} parts=${(d.progress && d.progress.parts) || []} />
        <${PiecesLegend} parts=${(d.progress && d.progress.parts) || []} />
      </div>

      <div class="detail-sections">
        ${Section("downloads_detail_sec_transfer", [
          statRow("downloads_status_label", t("downloads_status_" + d.status), "downloads_detail_tip_status"),
          statRow("downloads_detail_completed", formatBytes(d.size_done) + " (" + formatPercent(d.progress && d.progress.percent) + ")", "downloads_detail_tip_completed"),
          statRow("downloads_speed", formatSpeed(d.speed_bps), "downloads_detail_tip_speed"),
          statRow("downloads_detail_eta", eta, "downloads_detail_tip_eta"),
          statRow("downloads_sources", (src.transferring || 0) + " / " + (src.total || 0), "downloads_detail_tip_sources"),
          statRow("downloads_size", formatBytes(d.size), "downloads_detail_tip_size"),
          statRow("downloads_detail_transferred", formatBytes(d.size_xfer), "downloads_detail_tip_transferred"),
        ])}
        ${Section("downloads_detail_sec_activity", [
          statRow("downloads_detail_active_time", formatDuration(d.download_active_time), "downloads_detail_tip_active_time"),
          statRow("downloads_detail_last_changed", fmtTs(d.last_changed), "downloads_detail_tip_last_changed"),
          statRow("downloads_detail_last_seen_complete", fmtTs(d.last_seen_complete), "downloads_detail_tip_last_seen_complete"),
          statRow("downloads_detail_queued", formatInt(d.queued_count), "downloads_detail_tip_queued"),
        ])}
        ${media ? Section("downloads_detail_sec_media", [
          media.title ? statRow("downloads_detail_media_title", media.title, "downloads_detail_tip_media_title") : null,
          media.artist ? statRow("downloads_detail_media_artist", media.artist, "downloads_detail_tip_media_artist") : null,
          media.album ? statRow("downloads_detail_media_album", media.album, "downloads_detail_tip_media_album") : null,
          media.length_s ? statRow("downloads_detail_media_length", formatDuration(media.length_s), "downloads_detail_tip_media_length") : null,
          media.bitrate ? statRow("downloads_detail_media_bitrate", formatInt(media.bitrate), "downloads_detail_tip_media_bitrate") : null,
          media.codec ? statRow("downloads_detail_media_codec", media.codec, "downloads_detail_tip_media_codec") : null,
        ].filter(Boolean)) : null}
        ${d.comment ? Section("downloads_detail_sec_comment", [
          statRow("downloads_detail_comment", d.comment, "downloads_detail_tip_comment"),
          d.rating ? statRow("downloads_detail_rating", formatInt(d.rating), "downloads_detail_tip_rating") : null,
        ].filter(Boolean)) : null}
        ${Section("downloads_detail_sec_parts", [
          statRow("downloads_detail_available_parts", formatInt(d.available_part_count) + " / " + formatInt(d.part_count), "downloads_detail_tip_available_parts"),
          statRow("downloads_detail_saved_ich", formatInt(d.saved_by_ich) + " " + t("downloads_detail_ich_unit"), "downloads_detail_tip_saved_ich"),
          statRow("downloads_detail_lost_corruption", formatBytes(d.lost_to_corruption), "downloads_detail_tip_lost_corruption"),
          statRow("downloads_detail_gained_compression", formatBytes(d.gained_by_compression), "downloads_detail_tip_gained_compression"),
        ])}
        ${Section("downloads_detail_sec_identity", [
          statRow("downloads_detail_hash", html`<span class="mono">${(d.hash || "").toUpperCase()}</span>`, "downloads_detail_tip_hash"),
          statRow("downloads_detail_met_file", d.met_file || "—", "downloads_detail_tip_met_file"),
        ])}
      </div>
    </div>`;
}

// Chunk map: one proportional slice per ~9.28 MB part, coloured by state.
// Canvas rather than N <div>s so files with hundreds/thousands of parts redraw
// cheaply on every live tick. Colours are read from the theme each draw.
function PiecesBar({ parts }) {
  const ref = useRef(null);
  const drawRef = useRef(null);
  const partsRef = useRef(parts);
  partsRef.current = parts;

  useEffect(() => {
    const canvas = ref.current;
    if (!canvas) return;
    const draw = () => {
      const cs = getComputedStyle(document.documentElement);
      const complete = cs.getPropertyValue("--ok").trim();
      const missing = cs.getPropertyValue("--bad").trim();
      // available parts fade from few-sources -> many-sources blue
      const availLo = toRGB(cs.getPropertyValue("--piece-avail-lo"));
      const availHi = toRGB(cs.getPropertyValue("--piece-avail"));
      const dpr = window.devicePixelRatio || 1;
      const w = Math.max(1, Math.round(canvas.clientWidth * dpr));
      const h = Math.max(1, Math.round(canvas.clientHeight * dpr));
      if (canvas.width !== w) canvas.width = w;
      if (canvas.height !== h) canvas.height = h;
      const ctx = canvas.getContext("2d");
      ctx.clearRect(0, 0, w, h);
      const list = partsRef.current || [];
      const n = list.length || 1;
      const pw = w / n;
      // When pieces are wide enough, leave a 1px gap so each piece is
      // individually countable; when many/thin, overdraw to avoid seams and
      // let them blend into a continuous availability bar (the cleared track
      // background shows through the gaps).
      const gap = pw >= 6 * dpr ? Math.max(1, Math.round(dpr)) : 0;
      for (let i = 0; i < list.length; i++) {
        const p = list[i];
        let fill;
        if (p.state === "complete") fill = complete;
        else if (p.state === "incomplete") {
          const frac = Math.min(1, Math.max(0, ((p.sources || 1) - 1) / (AVAIL_FULL - 1)));
          fill = mix(availLo, availHi, frac);
        } else fill = missing;
        ctx.fillStyle = fill;
        const x0 = Math.round(i * pw), x1 = Math.round((i + 1) * pw);
        const width = gap ? Math.max(1, x1 - x0 - gap) : (x1 - x0) + 1;
        ctx.fillRect(x0, 0, width, h);
      }
    };
    drawRef.current = draw;
    draw();
    // Colours are read from CSS vars on every draw, so a theme switch only
    // needs a redraw (mirrors charts.js).
    const ro = new ResizeObserver(draw);
    ro.observe(canvas);
    const mo = new MutationObserver(draw);
    mo.observe(document.documentElement, { attributes: true, attributeFilter: ["data-theme"] });
    const mq = window.matchMedia("(prefers-color-scheme: dark)");
    mq.addEventListener("change", draw);
    return () => { ro.disconnect(); mo.disconnect(); mq.removeEventListener("change", draw); };
  }, []);

  useEffect(() => { drawRef.current && drawRef.current(); }, [parts]);

  return html`<div class="pieces-bar"><canvas ref=${ref}></canvas></div>`;
}

function PiecesLegend({ parts }) {
  const counts = { complete: 0, incomplete: 0, missing: 0 };
  for (const p of parts) counts[p.state] = (counts[p.state] || 0) + 1;
  const chip = (v) => html`<span class="legend-chip" style=${{ background: "var(" + v + ")" }}></span>`;
  // One line: three flat state chips (green/red/blue), then the availability
  // gradient scale grouped right after "Available" (it explains that blue's
  // shade encodes source count) — keeps the row compact and the chips uniform.
  return html`
    <div class="chart-legend pieces-legend">
      <span class="legend-item">${chip("--ok")} ${t("downloads_detail_part_complete")} <b>${counts.complete}</b></span>
      <span class="legend-item">${chip("--bad")} ${t("downloads_detail_part_missing")} <b>${counts.missing}</b></span>
      <span class="legend-item" title=${t("downloads_detail_avail_hint")}>
        ${chip("--piece-avail")} ${t("downloads_detail_part_incomplete")} <b>${counts.incomplete}</b>
        ${counts.incomplete > 0 ? html`
          <span class="pieces-scale">
            <small>(${t("downloads_detail_avail_fewer")}</small>
            <span class="pieces-scale-bar"></span>
            <small>${t("downloads_detail_avail_more")})</small>
          </span>` : null}
      </span>
      <span class="pieces-total">${tn("downloads_detail_pieces_count", parts.length)}</span>
    </div>`;
}
