// Shared-files detail panel: the per-file info shown below the shared table
// when a row is clicked. Data comes from GET shared/{hash} (every list field +
// the detail-only fields). It re-fetches on each live tick of the shared store
// so the transfer/request counters stay current while the panel is open (GET is
// ETag-cached, so unchanged frames short-circuit to a 304 + the cached body).
// Mirrors download-detail.js, but the pieces graph here is an *availability*
// bar, not a progress bar: a shared file is 100% local by definition, so what
// `parts[].sources` describes is how well each part is replicated across the
// network (red = no other peer has it). Absent until the daemon's RLE decode
// for this file has landed. While a re-hash runs the hashing bar replaces it,
// the same precedence CSharedFilesCtrl::GetItemBarFill applies.

import { api } from "../api.js";
import { html, useState, useEffect, useStore } from "../dom.js";
import { Placeholder, PiecesBar, PiecesLegend, toast, confirmDialog, Section, statRow, IdentityLine, copyText, Tabs, CommentEditor, RenameForm } from "../components.js";
import { formatBytes, formatInt, formatDuration, formatSpeed, formatTimestamp, twin } from "../format.js";
import { FileClients, HIDDEN_EVERYWHERE } from "./client-table.js";
import { t, terr } from "../i18n.js";

const PRIORITIES = ["very_low", "low", "normal", "high", "release"];

// Peers of a shared file: show the upload-side columns, keep the download ones
// (and the redundant per-row file name) one click away in the column picker.
const SH_HIDDEN = [...HIDDEN_EVERYWHERE, "file", "dl_state", "dl_speed", "downloaded", "dl_session", "remote_rank"];

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
  const [tab, setTab] = useState("details");

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
  if (!detail) return html`<div class="detail-panel"><${Placeholder} kind="loading">${t("shared_detail_loading")}<//></div>`;

  const s = detail;
  const media = s.media;

  const copy = (text) => copyText(text)
    .then(() => toast(t("downloads_detail_copied"), "success"))
    .catch(() => toast(t("downloads_detail_copy_failed"), "error"));

  // POST answers 202 as soon as amuled queues the task, so the toast can only
  // say the check *started*: the verdict is a log line (ThreadTasks.cpp
  // PrintReport), never a REST response. The 409 for a file that turned
  // partfile between the fetch and the click needs no branch -- terr() maps it.
  const verify = async () => {
    if (!(await confirmDialog(t("shared_verify_confirm", { name: s.name })))) return;
    try {
      await api.post("shared/" + s.hash + "/verify");
      toast(t("shared_verify_started"), "info");
    } catch (e) { toast(terr(e), "error"); }
  };

  return html`
    <div class="detail-panel">
      <div class="detail-head">
        <div class="detail-titlebar">
          <h4 class="detail-name" title=${s.name}>${s.name}</h4>
        </div>
      </div>

      <${Tabs} tabs=${[
        { key: "details", label: t("detail_tab_details") },
        { key: "clients", label: t("detail_tab_clients") },
        { key: "filename", label: t("detail_tab_filename") },
        { key: "comments", label: t("detail_tab_comments") },
      ]} active=${tab} onSelect=${setTab} />

      <div class="detail-body">
      ${tab === "clients" ? html`
        <${FileClients} hash=${s.hash} prefsKey="shared_clients" defaultHidden=${SH_HIDDEN}
                        defaultSort="uploaded" />
      ` : tab === "comments" ? html`
        <div class="detail-comments">
          <${CommentEditor} key=${s.hash} hash=${s.hash} kind="shared" comment=${s.comment} rating=${s.rating} />
        </div>
      ` : tab === "filename" ? html`
        <div class="detail-comments">
          <${RenameForm} key=${s.hash} hash=${s.hash} kind="shared" name=${s.name} />
        </div>
      ` : html`
      <div class="detail-sections">
        ${s.hashing_progress > 0 && s.part_count ? html`
        <div class="detail-progress">
          <${PiecesBar} mode="hashing" total=${s.part_count} hashed=${s.hashing_progress} />
          <${PiecesLegend} mode="hashing" total=${s.part_count} hashed=${s.hashing_progress} />
        </div>` : s.parts && s.parts.length ? html`
        <div class="detail-progress">
          <${PiecesBar} mode="availability" parts=${s.parts} />
          <${PiecesLegend} mode="availability" parts=${s.parts} />
        </div>` : null}
        ${Section([
          statRow("shared_size", formatBytes(s.size), "shared_detail_tip_size"),
          statRow("shared_detail_uploaded", twin(s.xfer, "session", "total", formatBytes), "shared_detail_tip_uploaded"),
          statRow("shared_detail_upload_speed", formatSpeed(s.upload_speed_bps), "shared_detail_tip_upload_speed"),
          statRow("shared_detail_uploading", formatInt(s.uploading), "shared_detail_tip_uploading"),
          statRow("shared_detail_last_upload", formatTimestamp(s.last_upload), "shared_detail_tip_last_upload"),
          statRow("shared_detail_shared_since", formatTimestamp(s.shared_since), "shared_detail_tip_shared_since"),
          statRow("shared_detail_requested", twin(s.requests, "session", "total", formatInt), "shared_detail_tip_requested"),
          statRow("shared_detail_accepted", twin(s.accepts, "session", "total", formatInt), "shared_detail_tip_accepted"),
          statRow("shared_detail_share_ratio", (Number(s.share_ratio) || 0).toFixed(2), "shared_detail_tip_share_ratio"),
          statRow("shared_detail_complete_src", completeSources(s), "shared_detail_tip_complete_src"),
        ], "shared_detail_group_sharing")}
        ${Section([
          statRow("shared_priority", prioLabel(s), "shared_detail_tip_priority"),
          statRow("downloads_detail_queued", formatInt(s.queued_count), "downloads_detail_tip_queued"),
          statRow("shared_detail_file_type", s.file_type || "—", "shared_detail_tip_file_type"),
        ], "shared_detail_group_file", html`
          <button class="btn btn-sm admin-only" type="button" disabled=${!!s.incomplete}
                  title=${t(s.incomplete ? "shared_verify_tip_partfile" : "shared_verify_tip")}
                  onClick=${verify}>
            ${t("shared_verify")}
          </button>`)}
        ${media ? Section([
          media.title ? statRow("downloads_detail_media_title", media.title, "downloads_detail_tip_media_title") : null,
          media.artist ? statRow("downloads_detail_media_artist", media.artist, "downloads_detail_tip_media_artist") : null,
          media.album ? statRow("downloads_detail_media_album", media.album, "downloads_detail_tip_media_album") : null,
          media.length_s ? statRow("downloads_detail_media_length", formatDuration(media.length_s), "downloads_detail_tip_media_length") : null,
          media.bitrate ? statRow("downloads_detail_media_bitrate", formatInt(media.bitrate), "downloads_detail_tip_media_bitrate") : null,
          media.codec ? statRow("downloads_detail_media_codec", media.codec, "downloads_detail_tip_media_codec") : null,
        ].filter(Boolean), "downloads_detail_group_media") : null}
        ${IdentityLine({ file: s, copy, titleKey: "downloads_detail_group_identity", extra: [
          statRow("shared_detail_path", s.path || "—", "shared_detail_tip_path"),
          statRow("shared_detail_parts", formatInt(s.part_count), "shared_detail_tip_parts"),
        ] })}
      </div>`}
      </div>
    </div>`;
}
