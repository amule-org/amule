// Shared-files detail panel: the per-file info shown below the shared table
// when a row is clicked. Data comes from GET shared/{hash} (every list field +
// the detail-only fields). It re-fetches on each live tick of the shared store
// so the transfer/request counters stay current while the panel is open (GET is
// ETag-cached, so unchanged frames short-circuit to a 304 + the cached body).
// Mirrors download-detail.js but shared files have no progress.parts, so there
// is no pieces graph.

import { api } from "../api.js";
import { html, useState, useEffect, useStore } from "../dom.js";
import { Placeholder, toast, Section, statRow, magnetLink, copyText } from "../components.js";
import { formatBytes, formatInt, formatDuration, twin } from "../format.js";
import { Icon } from "../icons.js";
import { t } from "../i18n.js";

const PRIORITIES = ["very_low", "low", "normal", "high", "release"];

// Human upload-priority label, matching the shared list (auto shows the
// derived level in parentheses).
function prioLabel(s) {
  const base = PRIORITIES.includes(s.priority) ? t("shared_prio_" + s.priority) : s.priority;
  return s.priority_auto ? t("shared_prio_auto") + " (" + base + ")" : base;
}

// Estimated peers with a complete copy, formatted like the desktop's
// "Complete Sources" column (SharedFilesCtrl.cpp): "< high" while the low
// bound is still 0, "low" once both bounds agree, else the "low – high"
// estimate range.
function completeSources(s) {
  const r = s.complete_sources_range || { low: 0, high: 0 };
  if (r.low === 0) return r.high ? "< " + formatInt(r.high) : "0";
  if (r.low === r.high) return formatInt(r.low);
  return formatInt(r.low) + " – " + formatInt(r.high);
}

export function SharedDetail({ hash }) {
  const shared = useStore("shared") || []; // live tick source (SSE)
  const [detail, setDetail] = useState(null);
  const [gone, setGone] = useState(false);

  useEffect(() => {
    if (!hash) { setDetail(null); return; }
    let alive = true;
    api.get("shared/" + hash)
      .then((d) => { if (alive) { setDetail(d); setGone(false); } })
      .catch(() => { if (alive) setGone(true); });
    return () => { alive = false; };
  }, [hash, shared]);

  if (!hash) return null;
  if (gone) return html`<div class="detail-panel"><${Placeholder} kind="info">${t("shared_detail_gone")}<//></div>`;
  if (!detail) return html`<div class="detail-panel"><${Placeholder} kind="info">${t("shared_detail_loading")}<//></div>`;

  const s = detail;
  const media = s.media;

  const copy = (text) => copyText(text)
    .then(() => toast(t("downloads_detail_copied"), "success"))
    .catch(() => toast(t("downloads_detail_copy_failed"), "error"));

  return html`
    <div class="detail-panel">
      <div class="detail-head">
        <div class="detail-titlebar">
          <h4 class="detail-name" title=${s.name}>${s.name}</h4>
          <div class="detail-actions">
            <button class="btn btn-sm" type="button" onClick=${() => copy(s.ed2k_link)}>
              <${Icon} name="copy" /> ${t("downloads_detail_copy_ed2k")}
            </button>
            <button class="btn btn-sm" type="button" onClick=${() => copy(magnetLink(s))}>
              <${Icon} name="copy" /> ${t("downloads_detail_copy_magnet")}
            </button>
          </div>
        </div>
      </div>

      <div class="detail-sections">
        ${Section("shared_detail_sec_sharing", [
          statRow("shared_size", formatBytes(s.size), "shared_detail_tip_size"),
          statRow("shared_detail_uploaded", twin(s.xfer, "session", "total", formatBytes), "shared_detail_tip_uploaded"),
          statRow("shared_detail_requested", twin(s.requests, "session", "total", formatInt), "shared_detail_tip_requested"),
          statRow("shared_detail_accepted", twin(s.accepts, "session", "total", formatInt), "shared_detail_tip_accepted"),
          statRow("shared_detail_share_ratio", (Number(s.share_ratio) || 0).toFixed(2), "shared_detail_tip_share_ratio"),
          statRow("shared_detail_complete_src", completeSources(s), "shared_detail_tip_complete_src"),
        ])}
        ${Section("downloads_detail_sec_activity", [
          statRow("shared_priority", prioLabel(s), "shared_detail_tip_priority"),
          statRow("downloads_detail_queued", formatInt(s.queued_count), "downloads_detail_tip_queued"),
          statRow("shared_detail_file_type", s.file_type || "—", "shared_detail_tip_file_type"),
        ])}
        ${media ? Section("downloads_detail_sec_media", [
          media.title ? statRow("downloads_detail_media_title", media.title, "downloads_detail_tip_media_title") : null,
          media.artist ? statRow("downloads_detail_media_artist", media.artist, "downloads_detail_tip_media_artist") : null,
          media.album ? statRow("downloads_detail_media_album", media.album, "downloads_detail_tip_media_album") : null,
          media.length_s ? statRow("downloads_detail_media_length", formatDuration(media.length_s), "downloads_detail_tip_media_length") : null,
          media.bitrate ? statRow("downloads_detail_media_bitrate", formatInt(media.bitrate), "downloads_detail_tip_media_bitrate") : null,
          media.codec ? statRow("downloads_detail_media_codec", media.codec, "downloads_detail_tip_media_codec") : null,
        ].filter(Boolean)) : null}
        ${s.comment ? Section("downloads_detail_sec_comment", [
          statRow("downloads_detail_comment", s.comment, "downloads_detail_tip_comment"),
          s.rating ? statRow("downloads_detail_rating", formatInt(s.rating), "downloads_detail_tip_rating") : null,
        ].filter(Boolean)) : null}
        ${Section("downloads_detail_sec_identity", [
          statRow("downloads_detail_hash", html`<span class="mono">${(s.hash || "").toUpperCase()}</span>`, "downloads_detail_tip_hash"),
          statRow("shared_detail_path", s.path || "—", "shared_detail_tip_path"),
          statRow("shared_detail_parts", formatInt(s.part_count), "shared_detail_tip_parts"),
        ])}
      </div>
    </div>`;
}
