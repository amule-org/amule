// Downloads detail panel: the per-file info + pieces graph shown below the
// downloads table when a row is clicked. Data comes from GET downloads/{hash}
// (every list field + the detail-only fields + progress.parts). It re-fetches
// on each live tick of the downloads store so %, ETA, speed and the pieces
// graph stay current while the panel is open (GET is ETag-cached, so unchanged
// frames short-circuit to a 304 + the cached body).

import { api } from "../api.js";
import { store } from "../store.js";
import { html, useState, useEffect, useStore } from "../dom.js";
import { ProgressBar, Placeholder, PiecesBar, PiecesLegend, toast, confirmDialog, Section, statRow, IdentityLine, copyText, Tabs, CommentEditor, CommentsList, RenameForm, PRIORITIES, prioValue, prioLabel } from "../components.js";
import { formatBytes, formatSpeed, formatDuration, formatInt, formatPercent, formatTimestamp } from "../format.js";
import { Icon } from "../icons.js";
import { FileClients, HIDDEN_EVERYWHERE } from "./client-table.js";
import { categoryName, categoryOptions } from "./categories.js";
import { t, tn, terr } from "../i18n.js";

// Peers of a download: show the download-side columns, keep the upload ones
// (and the redundant per-row file name) one click away in the column picker.
const DL_HIDDEN = [...HIDDEN_EVERYWHERE, "file", "ul_state", "ul_speed", "uploaded", "ul_session", "queue_pos", "score"];

export function DownloadDetail({ hash, isGuest, categories = [], onPatch, onDelete, onClear }) {
  const downloads = useStore("downloads") || []; // live tick source (SSE ~500ms)
  const [detail, setDetail] = useState(null);
  const [gone, setGone] = useState(false);
  const [tab, setTab] = useState("details");
  const [reload, setReload] = useState(0); // bumped after a rename to refresh the name

  // The live re-fetch feeds the Details tab (%, speed, ETA, pieces). The
  // Comments tab needs none of that, so only tick there — elsewhere the detail
  // loads once per file (and once more when returning to Details).
  const liveTick = tab === "details" ? downloads : 0;
  useEffect(() => {
    if (!hash) { setDetail(null); return; }
    let alive = true;
    api.get("downloads/" + hash)
      .then((d) => { if (alive) { setDetail(d); setGone(false); } })
      .catch(() => { if (alive) setGone(true); });
    return () => { alive = false; };
  }, [hash, liveTick, reload]);

  if (!hash) return null;
  if (gone) return html`<div class="detail-panel"><${Placeholder} kind="info">${t("downloads_detail_gone")}<//></div>`;
  if (!detail) return html`<div class="detail-panel"><${Placeholder} kind="loading">${t("downloads_detail_loading")}<//></div>`;

  const d = detail;
  const src = d.sources || {};
  const media = d.media;
  // Empty when the daemon has sent no chunk map: the bar is skipped entirely
  // rather than drawn as an empty track claiming "0 pieces", matching the
  // shared panel's handling of a share whose RLE decode has not landed. A
  // running re-hash replaces this bar (the chunk map is stale by definition
  // then), but not the percent bar above -- that is the download's own
  // completeness.
  const parts = (d.progress && d.progress.parts) || [];
  const eta = d.remaining_time == null ? "—" : formatDuration(d.remaining_time);

  const copy = (text) => copyText(text)
    .then(() => toast(t("downloads_detail_copied"), "success"))
    .catch(() => toast(t("downloads_detail_copy_failed"), "error"));

  // `a4af_auto` is not part of EqualDownload (EventDiff.cpp), so changing it emits
  // no download_updated and the tick-driven re-fetch never sees it — bump `reload`.
  const a4af = (action) => api.post("downloads/" + hash + "/a4af", { action })
    .then(() => { toast(t("downloads_a4af_done"), "success"); setReload((n) => n + 1); })
    .catch((e) => toast(terr(e), "error"));

  // The auto flag is set, not toggled: send the value the button is moving to
  // rather than asking the daemon to flip whatever it currently holds. A retry
  // then lands on the same value instead of undoing the press.
  const setA4afAuto = (value) => api.patch("downloads/" + hash, { a4af_auto: value })
    .then(() => { toast(t("downloads_a4af_done"), "success"); setReload((n) => n + 1); })
    .catch((e) => toast(terr(e), "error"));

  return html`
    <div class="detail-panel">
      <div class="detail-head">
        <div class="detail-titlebar">
          <h4 class="detail-name" title=${d.name}>${d.name}</h4>
        </div>
        ${onPatch ? html`<${DetailActions} d=${d} isGuest=${isGuest} categories=${categories}
                                           onPatch=${onPatch} onDelete=${onDelete} onClear=${onClear} />` : null}
      </div>

      <${Tabs} tabs=${[
        { key: "details", label: t("detail_tab_details") },
        { key: "clients", label: t("detail_tab_clients") },
        { key: "filename", label: t("detail_tab_filename") },
        { key: "comments", label: t("detail_tab_comments") },
      ]} active=${tab} onSelect=${setTab} />

      <div class="detail-body">
      ${tab === "clients" ? html`
        <${FileClients} hash=${d.hash} prefsKey="download_clients" defaultHidden=${DL_HIDDEN}
                        defaultSort="downloaded" />
      ` : tab === "comments" ? html`
        <${DownloadComments} hash=${d.hash} comment=${d.comment} rating=${d.rating}
                             running=${!!(downloads.find((x) => x.hash === d.hash) || {}).kad_comment_search_running}
                             parts=${parts} />
      ` : tab === "filename" ? html`
        <${DownloadFilenames} hash=${d.hash} name=${d.name}
                              onRenamed=${() => setReload((n) => n + 1)} />
      ` : html`
      <div class="detail-sections">
        <div class="detail-progress">
          <${ProgressBar} percent=${d.progress && d.progress.percent} />
          ${d.hashing_progress > 0 && d.part_count ? html`
            <${PiecesBar} mode="hashing" total=${d.part_count} hashed=${d.hashing_progress} />
            <${PiecesLegend} mode="hashing" total=${d.part_count} hashed=${d.hashing_progress} />`
          : parts.length ? html`
            <${PiecesBar} parts=${parts} />
            <${PiecesLegend} parts=${parts} />` : null}
        </div>
        ${Section([
          statRow("downloads_status_label", t("downloads_status_" + d.status), "downloads_detail_tip_status"),
          statRow("downloads_detail_completed", formatBytes(d.size_done) + " (" + formatPercent(d.progress && d.progress.percent) + ")", "downloads_detail_tip_completed"),
          statRow("downloads_speed", formatSpeed(d.speed_bps), "downloads_detail_tip_speed"),
          statRow("downloads_detail_eta", eta, "downloads_detail_tip_eta"),
          statRow("downloads_sources", (src.transferring || 0) + " / " + (src.total || 0), "downloads_detail_tip_sources"),
          statRow("downloads_size", formatBytes(d.size), "downloads_detail_tip_size"),
          statRow("downloads_detail_transferred", formatBytes(d.size_xfer), "downloads_detail_tip_transferred"),
        ], "downloads_detail_group_transfer")}
        ${Section([
          statRow("downloads_detail_active_time", formatDuration(d.download_active_time), "downloads_detail_tip_active_time"),
          statRow("downloads_detail_last_changed", formatTimestamp(d.last_changed), "downloads_detail_tip_last_changed"),
          statRow("downloads_detail_last_seen_complete", formatTimestamp(d.last_seen_complete), "downloads_detail_tip_last_seen_complete"),
          statRow("downloads_detail_queued", formatInt(d.queued_count), "downloads_detail_tip_queued"),
        ], "downloads_detail_group_history")}
        ${media ? Section([
          media.title ? statRow("downloads_detail_media_title", media.title, "downloads_detail_tip_media_title") : null,
          media.artist ? statRow("downloads_detail_media_artist", media.artist, "downloads_detail_tip_media_artist") : null,
          media.album ? statRow("downloads_detail_media_album", media.album, "downloads_detail_tip_media_album") : null,
          media.length_s ? statRow("downloads_detail_media_length", formatDuration(media.length_s), "downloads_detail_tip_media_length") : null,
          media.bitrate ? statRow("downloads_detail_media_bitrate", formatInt(media.bitrate), "downloads_detail_tip_media_bitrate") : null,
          media.codec ? statRow("downloads_detail_media_codec", media.codec, "downloads_detail_tip_media_codec") : null,
        ].filter(Boolean), "downloads_detail_group_media") : null}
        ${Section([
          statRow("downloads_detail_available_parts", formatInt(d.available_part_count) + " / " + formatInt(d.part_count), "downloads_detail_tip_available_parts"),
          statRow("downloads_detail_saved_ich", formatInt(d.saved_by_ich) + " " + t("downloads_detail_ich_unit"), "downloads_detail_tip_saved_ich"),
          statRow("downloads_detail_lost_corruption", formatBytes(d.lost_to_corruption), "downloads_detail_tip_lost_corruption"),
          statRow("downloads_detail_gained_compression", formatBytes(d.gained_by_compression), "downloads_detail_tip_gained_compression"),
        ], "downloads_detail_group_integrity")}
        ${Section(
          [statRow("downloads_sources", formatInt(src.a4af || 0), "downloads_detail_tip_a4af")],
          "downloads_detail_group_a4af",
          html`
            <button class="btn btn-sm admin-only" type="button" title=${t("downloads_a4af_tip_swap_this")}
                    onClick=${() => a4af("swap_this")}>
              ${t("downloads_a4af_swap_this")}
            </button>
            <button class="btn btn-sm admin-only" type="button" title=${t("downloads_a4af_tip_swap_others")}
                    onClick=${() => a4af("swap_others")}>
              ${t("downloads_a4af_swap_others")}
            </button>
            <button class=${"btn btn-sm admin-only" + (d.a4af_auto ? " btn-primary" : "")} type="button"
                    title=${t("downloads_a4af_tip_auto")}
                    aria-pressed=${!!d.a4af_auto} onClick=${() => setA4afAuto(!d.a4af_auto)}>
              ${t("downloads_a4af_auto")}
            </button>`)}
        ${IdentityLine({ file: d, copy, titleKey: "downloads_detail_group_identity", extra: [
          statRow("downloads_detail_path", d.path || "—", "downloads_detail_tip_path"),
          statRow("downloads_detail_met_file", d.met_file || "—", "downloads_detail_tip_met_file"),
        ] })}
      </div>`}
      </div>
    </div>`;
}

// Per-file action bar, pinned in the panel head above the tab strip so it stays
// reachable from every tab (and, on phones, at the top of the full-screen sheet).
function DetailActions({ d, isGuest, categories, onPatch, onDelete, onClear }) {
  const inactive = d.status === "paused" || d.status === "stopped";
  const canStop = d.status !== "stopped" && d.status !== "completed" && d.status !== "completing";
  // Completed rejects DELETE (409 completed_use_clear_completed): offer Clear.
  const done = d.status === "completed";

  const clear = async () => {
    if (!(await confirmDialog(t("downloads_confirm_clear_this", { name: d.name })))) return;
    onClear(d.hash);
  };

  // admin-only per button, not on the bar: guests keep the priority/category readout.
  return html`
    <div class="detail-actions">
      <button class="btn btn-sm admin-only" type="button"
              onClick=${() => onPatch(d.hash, { status: inactive ? "resumed" : "paused" })}>
        <${Icon} name=${inactive ? "play" : "pause"} /> ${inactive ? t("downloads_resume") : t("downloads_pause")}
      </button>
      ${canStop ? html`
        <button class="btn btn-sm admin-only" type="button" onClick=${() => onPatch(d.hash, { status: "stopped" })}>
          <${Icon} name="stop" /> ${t("downloads_stop")}
        </button>` : null}
      ${done ? html`
        <button class="btn btn-sm admin-only" type="button" onClick=${clear}>
          <${Icon} name="cancel" /> ${t("downloads_clear_this")}
        </button>` : html`
        <button class="btn btn-sm btn-danger admin-only" type="button" onClick=${() => onDelete(d)}>
          <${Icon} name="cancel" /> ${t("downloads_cancel")}
        </button>`}
      <div class="field field-inline" title=${t("downloads_detail_tip_priority")}>
        <label>${t("downloads_priority")}</label>
        ${isGuest ? html`<b>${prioLabel(d)}</b>` : html`
          <select class="input input-sm" value=${prioValue(d)}
                  onChange=${(e) => onPatch(d.hash, { priority: e.target.value })}>
            ${PRIORITIES.map(([v, l]) => html`<option value=${v}>${v === "auto" && d.priority_auto ? prioLabel(d) : l}</option>`)}
          </select>`}
      </div>
      <div class="field field-inline" title=${t("downloads_detail_tip_category")}>
        <label>${t("downloads_category")}</label>
        ${isGuest ? html`<b>${categoryName(categories, d.category)}</b>` : html`
          <select class="input input-sm" value=${d.category}
                  onChange=${(e) => onPatch(d.hash, { category: Number(e.target.value) })}>
            ${categoryOptions(categories)}
          </select>`}
      </div>
    </div>`;
}

// The Comments tab body: your own comment/rating editor, a "Get from Kad"
// trigger, and the per-source comments list (GET downloads/{hash}/comments —
// includes any retrieved Kad notes). Fetched once per file, then kept current
// by the comments_updated SSE event, whose payload is this same body: Kad notes
// arriving mid-search and source comments both land without polling. `running`
// comes from the downloads store (the flag is part of EqualDownload, so its
// start -> finish edge arrives as download_updated).
function DownloadComments({ hash, comment, rating, running, parts }) {
  const [data, setData] = useState(null);

  useEffect(() => {
    let alive = true;
    api.get("downloads/" + hash + "/comments")
      .then((d) => { if (alive) setData(d); })
      .catch(() => { if (alive) setData({ count: 0, comments: [] }); });
    const off = store.subscribe("comments:updated", (p) => {
      if (alive && p && p.hash === hash) setData(p);
    });
    return () => { alive = false; off(); };
  }, [hash]);

  const list = (data && data.comments) || [];
  // The daemon only accepts a comment/rating on a *shared* file, i.e. a
  // partfile with >= 1 complete part; otherwise PATCH returns 409 not_shared.
  // Gate the editor on the same condition instead of letting the save fail.
  const canComment = (parts || []).some((p) => p.state === "complete");

  const getKad = () => api.post("downloads/" + hash + "/comments")
    .then(() => toast(t("comments_kad_started"), "info"))
    .catch((e) => toast(e.code === "amuled_rejected" ? t("comments_kad_error") : terr(e), "error"));

  return html`
    <div class="detail-comments">
      <${CommentEditor} key=${hash} hash=${hash} kind="downloads" comment=${comment} rating=${rating}
                        disabled=${!canComment} disabledHint=${t("comments_disabled_hint")} />
      <div class="comments-head">
        <span class="comments-count">${tn("comments_count", list.length)}</span>
        <button class="btn btn-sm admin-only" type="button" onClick=${getKad} disabled=${running}>
          <${Icon} name=${running ? "polling" : "kad"} />
          ${running ? t("comments_kad_searching") : t("comments_get_kad")}
        </button>
      </div>
      <${CommentsList} comments=${list} />
    </div>`;
}

// The File names tab: the distinct names this download's sources report for it
// (GET downloads/{hash}/filenames), plus a rename form. Admins can rename the
// file to an arbitrary name or "take over" any source-reported name (the
// desktop Takeover flow). Loads once per file — the source-name list doesn't
// change on every SSE tick.
function DownloadFilenames({ hash, name, onRenamed }) {
  const [data, setData] = useState(null);

  useEffect(() => {
    let alive = true;
    api.get("downloads/" + hash + "/filenames")
      .then((d) => { if (alive) setData(d); })
      .catch(() => { if (alive) setData({ filenames: [] }); });
    return () => { alive = false; };
  }, [hash]);

  // Server returns map-iteration order; sort by popularity then name.
  const list = ((data && data.filenames) || [])
    .slice()
    .sort((a, b) => (b.count - a.count) || a.name.localeCompare(b.name));

  const takeover = (n) => api.patch("downloads/" + hash, { name: n })
    .then(() => { toast(t("rename_saved"), "success"); if (onRenamed) onRenamed(); })
    .catch((e) => toast(terr(e), "error"));

  return html`
    <div class="detail-comments">
      <${RenameForm} key=${hash} hash=${hash} kind="downloads" name=${name} onSaved=${onRenamed} />
      ${list.length ? html`
        <table class="comments-list">
          <thead><tr>
            <th>${t("comments_col_filename")}</th>
            <th>${t("filename_col_count")}</th>
            <th class="admin-only"></th>
          </tr></thead>
          <tbody>
            ${list.map((f, i) => html`
              <tr key=${i}>
                <td class="comments-fname" title=${f.name}>${f.name}</td>
                <td>${f.count}</td>
                <td class="admin-only">
                  <button class="btn btn-sm" type="button" onClick=${() => takeover(f.name)}>
                    ${t("filename_takeover")}
                  </button>
                </td>
              </tr>`)}
          </tbody>
        </table>` : html`<${Placeholder} kind="info">${t("filename_none")}<//>`}
    </div>`;
}

