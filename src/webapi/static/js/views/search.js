// Search view: one tab per search, like the desktop's notebook. The rendering
// half only -- which searches are open, their results and their per-tab UI
// live in ../searches.js, because this view unmounts whenever the user
// navigates to another section and the tabs have to survive that.

import { api } from "../api.js";
import { html, useState, useEffect, useStore } from "../dom.js";
import { Badge, Placeholder, listPlaceholder, Tabs, CommentsList, ratingLabel, toast } from "../components.js";
import { VirtualTable, sortRows, textMatcher, useTablePrefs, ColumnPicker } from "../table.js";
import { formatBytes, formatDuration, formatInt } from "../format.js";
import { Icon } from "../icons.js";
import { searches } from "../searches.js";
import { t, tn, terr } from "../i18n.js";

const SIZE_UNITS = { B: 1, KiB: 1024, MiB: 1048576, GiB: 1073741824 };
// [API value, label key] — the value goes to the backend verbatim.
const FILE_TYPES = [
  ["", "search_ftype_any"], ["Audio", "search_ftype_audio"], ["Video", "search_ftype_video"],
  ["Image", "search_ftype_image"], ["Document", "search_ftype_document"], ["Program", "search_ftype_program"],
  ["Archive", "search_ftype_archive"], ["CD/DVD", "search_ftype_cddvd"],
];
// A tab is as wide as its label, so a long query gets cut; the full string
// stays in the tab's title attribute.
const TAB_LABEL_MAX = 24;

export default function Search({ isGuest }) {
  const [query, setQuery] = useState("");
  const [type, setType] = useState("global");
  const [fileType, setFileType] = useState("");
  const [ext, setExt] = useState("");
  const [minAvail, setMinAvail] = useState("");
  const [minSize, setMinSize] = useState("");
  const [minUnit, setMinUnit] = useState("MiB");
  const [maxSize, setMaxSize] = useState("");
  const [maxUnit, setMaxUnit] = useState("MiB");
  const [categories, setCategories] = useState([]);

  // The registry publishes the light tab list; the heavy results array rides
  // its own key, consumed by the pane below.
  const reg = useStore("searches") || { tabs: [], activeId: 0 };
  const active = reg.tabs.find((x) => x.id === reg.activeId) || null;

  useEffect(() => {
    // Idempotent: attaches the stream routing and adopts whatever the daemon
    // already holds (this browser's searches, another client's, or ones
    // restored across a restart).
    searches.ensure();
    // An unopened tab has no amuleapi slot, so no SSE refreshes its badge --
    // re-list on every mount instead. Debounced, so the first one is free.
    searches.nudgeAdopt();
    api.get("categories").then((r) => setCategories(r.categories || [])).catch(() => {});
  }, []);

  const sizeBytes = (v, unit) => { const n = Number(v); return (!n || n < 0) ? 0 : Math.round(n * SIZE_UNITS[unit]); };

  const startSearch = async (e) => {
    e.preventDefault();
    const q = query.trim();
    if (!q) { toast(t("search_toast_enter_search_terms"), "warn"); return; }
    const body = { query: q, type };
    if (fileType) body.file_type = fileType;
    if (ext.trim()) body.extension = ext.trim();
    if (Number(minAvail) > 0) body.min_avail = Number(minAvail);
    const mn = sizeBytes(minSize, minUnit), mx = sizeBytes(maxSize, maxUnit);
    if (mn) body.min_size = mn;
    if (mx) body.max_size = mx;
    try {
      // Every start opens its own tab; the query stays in the box so refining
      // and re-running is one edit.
      await searches.start(body);
      toast(t("search_toast_search_started"), "success");
    } catch (err) { toast(terr(err) || t("search_error"), "error"); }
  };

  const unitSelect = (value, onChange) => html`
    <select class="input input-sm" value=${value} onChange=${onChange}>
      ${Object.keys(SIZE_UNITS).map((u) => html`<option value=${u}>${u}</option>`)}
    </select>`;

  const tabItems = reg.tabs.map((x) => {
    const full = x.label || x.query || ("#" + x.id);
    return {
      key: String(x.id),
      label: full.length > TAB_LABEL_MAX ? full.slice(0, TAB_LABEL_MAX - 1) + "…" : full,
      title: full + " · " + t("search_type_" + x.kind),
      badge: x.count,
      cls: x.state === "running" ? "running" : "",
      closeLabel: t("search_tab_close"),
    };
  });

  return html`
    <div class="fill-view">
    <form class="card search-form" onSubmit=${startSearch}>
      <div class="search-grid">
        ${field(t("search_query"), html`<input class="input" type="text" placeholder=${t("search_terms_ph")} required value=${query} onInput=${(e) => setQuery(e.target.value)} />`, "field-wide")}
        ${field(t("search_type"), html`<select class="input" value=${type} onChange=${(e) => setType(e.target.value)}>
          <option value="global">${t("search_type_global")}</option><option value="local">${t("search_type_local")}</option><option value="kad">${t("search_type_kad")}</option>
        </select>`)}
        ${field(t("search_file_type"), html`<select class="input" value=${fileType} onChange=${(e) => setFileType(e.target.value)}>
          ${FILE_TYPES.map(([v, k]) => html`<option value=${v}>${t(k)}</option>`)}
        </select>`)}
        ${field(t("search_extension"), html`<input class="input" type="text" placeholder=${t("search_ext_ph")} value=${ext} onInput=${(e) => setExt(e.target.value)} />`)}
        ${field(t("search_min_availability"), html`<input class="input" type="number" min="0" placeholder="0" value=${minAvail} onInput=${(e) => setMinAvail(e.target.value)} />`)}
        ${field(t("search_min_size"), html`<div class="field-inline">
          <input class="input" type="number" min="0" value=${minSize} onInput=${(e) => setMinSize(e.target.value)} />
          ${unitSelect(minUnit, (e) => setMinUnit(e.target.value))}
        </div>`)}
        ${field(t("search_max_size"), html`<div class="field-inline">
          <input class="input" type="number" min="0" value=${maxSize} onInput=${(e) => setMaxSize(e.target.value)} />
          ${unitSelect(maxUnit, (e) => setMaxUnit(e.target.value))}
        </div>`)}
      </div>
      <div class="toolbar">
        <button class="btn btn-primary admin-only" type="submit">${t("search_search")}</button>
      </div>
      ${isGuest ? html`<p class="hint">${t("search_guest_readonly")}</p>` : null}
    </form>

    <section class="net-pane pane-fill">
      <${Tabs} cls="search-tabs" tabs=${tabItems} active=${String(reg.activeId)}
               onSelect=${(k) => searches.setActive(Number(k))}
               onClose=${(k) => searches.close(Number(k))}
               extra=${reg.tabs.length > 1 ? html`
                 <button class="btn btn-sm admin-only" type="button"
                         onClick=${() => searches.closeAll()}>${t("search_close_all")}</button>` : null} />
      <div class="net-pane-body">
        ${active
          ? html`<${ResultsPane} key=${active.id} tab=${active} categories=${categories} />`
          : html`<${Placeholder} kind="info">${t("search_no_tabs")}<//>`}
      </div>
    </section>
    </div>`;
}

// One search's results. Keyed on the tab, so switching tabs starts clean (no
// dialog left open on a hit from the search you just left) -- everything worth
// keeping already lives in the registry. Column prefs are stored per KIND: a
// browse listing leads with the remote folder, a query listing with sources.
function ResultsPane({ tab, categories }) {
  const rows = useStore("search:" + tab.id) || [];
  const [commentsFor, setCommentsFor] = useState(null);
  const browse = tab.kind === "browse";
  const { sortKey, sortDir, hidden, widths, toggleSort, toggleCol, setWidth, resetPrefs } =
    useTablePrefs(browse ? "search-browse" : "search", browse
      ? { sortKey: "directory", sortDir: 1, hidden: ["sources", "rating", "status", "length", "bitrate", "codec"] }
      : { sortKey: "sources", sortDir: -1, hidden: ["directory", "length", "bitrate", "codec"] });

  // Selection, filters and per-row choices belong to the TAB, not to this
  // component: switching tabs and coming back must restore them.
  const ui = searches.ui(tab.id) || { selection: new Set(), filter: "", filterHave: "all", cat: 0, rowCat: {}, rowEcid: {} };
  const setUi = (patch) => searches.setUi(tab.id, patch);
  // Per-row category, falling back to the tab's toolbar default.
  const catFor = (h) => (ui.rowCat[h] != null ? ui.rowCat[h] : Number(ui.cat) || 0);

  const visible = (all, filter, filterHave) => {
    const m = textMatcher(filter);
    let out = filter ? all.filter((r) => m(r.name)) : all;
    if (filterHave !== "all") out = out.filter((r) => (filterHave === "have") === !!r.already_have);
    return out;
  };
  const filtered = visible(rows, ui.filter, ui.filterHave);

  // A hidden row must not stay selected: "Download selected" would queue a
  // file the user cannot see. Prune as the filter changes rather than after.
  const prune = (sel, all) => {
    const vis = new Set(all.map((r) => r.hash));
    const next = new Set();
    sel.forEach((h) => vis.has(h) && next.add(h));
    return next;
  };
  const setFilter = (v) => setUi({ filter: v, selection: prune(ui.selection, visible(rows, v, ui.filterHave)) });
  const setFilterHave = (v) => setUi({ filterHave: v, selection: prune(ui.selection, visible(rows, ui.filter, v)) });

  const toggleRow = (hash, checked) => {
    const next = new Set(ui.selection);
    if (checked) next.add(hash); else next.delete(hash);
    setUi({ selection: next });
  };
  const toggleAll = (checked) => setUi({ selection: checked ? new Set(filtered.map((r) => r.hash)) : new Set() });

  const allSelected = filtered.length > 0 && filtered.every((r) => ui.selection.has(r.hash));
  const selectedCount = filtered.filter((r) => ui.selection.has(r.hash)).length;

  const downloadBody = (hash) => {
    const body = { category: catFor(hash) };
    // A grouped child is picked by its own ecid, which is what queues the file
    // under that advertised name instead of the aggregated one.
    const ecid = ui.rowEcid[hash];
    if (ecid) body.ecid = Number(ecid);
    return body;
  };
  const afterDownload = () => searches.refresh(tab.id); // status/already_have only move on a read

  const downloadSelected = async () => {
    const hashes = Array.from(ui.selection);
    if (!hashes.length) { toast(t("search_toast_no_results_selected"), "warn"); return; }
    try {
      await Promise.all(hashes.map((h) => api.post("search/results/" + h + "/download", downloadBody(h))));
      toast(tn("search_toast_added_downloads", hashes.length), "success");
      setUi({ selection: new Set() });
      afterDownload();
    } catch (e) { toast(terr(e) || t("search_error"), "error"); }
  };

  const downloadOne = async (h) => {
    try {
      await api.post("search/results/" + h + "/download", downloadBody(h));
      toast(tn("search_toast_added_downloads", 1), "success");
      afterDownload();
    } catch (e) { toast(terr(e) || t("search_error"), "error"); }
  };

  const relatedSearch = async () => {
    const hashes = Array.from(ui.selection);
    if (!hashes.length) { toast(t("search_toast_no_results_selected"), "warn"); return; }
    // One search over every selected hit, as the desktop does. Naming the tab
    // after one of several files would be a lie, so past one it counts them.
    const first = rows.find((r) => r.hash === hashes[0]);
    const label = hashes.length === 1 && first ? first.name
      : t("search_related_tab_fmt", { count: hashes.length });
    try { await searches.related(hashes, label); }
    catch (e) { toast(terr(e) || t("search_error"), "error"); }
  };

  const catOptions = () => html`
    <option value=${0}>${t("search_category_none")}</option>
    ${categories.filter((c) => c.index !== 0).map((c) => html`<option value=${c.index}>${c.name || ("#" + c.index)}</option>`)}`;

  const columns = [
    { always: true, label: html`<input type="checkbox" title=${t("search_select_all")} checked=${allSelected}
                         onChange=${(e) => toggleAll(e.target.checked)} />`, width: "40px",
      cell: (r) => html`<input type="checkbox" checked=${ui.selection.has(r.hash)} onChange=${(e) => toggleRow(r.hash, e.target.checked)} />` },
    { key: "name", always: true, label: t("search_name"), cls: "name", sortable: true,
      sortVal: (r) => (r.name || "").toLowerCase(),
      // already_have is signalled by the row background (.row-have), not a badge.
      // A hit advertised under several filenames picks one here, and that is
      // what the download uses -- single row and bulk alike, via the ecid.
      cell: (r) => {
        // The core names a group after its most-sourced child
        // (CSearchFile::UpdateParent), so one child always repeats the parent's
        // name; the default option below already IS that name.
        const kids = (r.children || []).filter((c) => c.name !== r.name);
        if (!kids.length) return r.name;
        return html`
          <select class="input input-sm name-select" title=${t("search_alt_names_title")}
                  value=${ui.rowEcid[r.hash] || ""}
                  onChange=${(e) => setUi({ rowEcid: { ...ui.rowEcid, [r.hash]: e.target.value } })}>
            <option value="">${r.name}</option>
            ${kids.map((c) => html`<option value=${c.ecid}>${c.name}</option>`)}
          </select>`;
      } },
    { key: "size", label: t("search_size"), num: true, width: "110px", sortable: true,
      sortVal: (r) => r.size || 0, cell: (r) => formatBytes(r.size) },
    { key: "sources", label: t("search_sources"), num: true, width: "120px", sortable: true,
      sortVal: (r) => (r.sources && r.sources.total) || 0,
      // Total, plus the complete count in parentheses only when there IS one,
      // like the desktop (SearchListModel.cpp:242-246). A Kad hit never carries
      // one -- only ed2k servers send FT_COMPLETE_SOURCES -- so a fixed "0 / N"
      // would print a zero meaning "unknown", not "none".
      cell: (r) => {
        const src = r.sources || {};
        const total = src.total || 0, complete = src.complete || 0;
        const tip = complete ? t("search_title_sources_fmt", { total, complete }) : t("search_title_sources_total");
        const text = complete ? total + " (" + complete + ")" : String(total);
        return html`<span title=${tip}>${text}</span>`;
      } },
    { key: "rating", label: t("search_rating"), num: true, width: "90px", sortable: true,
      sortVal: (r) => r.rating || 0,
      // 0 means "nobody rated this", not "rated zero", so it renders as "—"
      // (the desktop blanks the cell, SearchListModel.cpp:268-277). A Kad hit is
      // always 0: TAG_FILERATING rides a Kad *note*, not a keyword result, so a
      // rating only ever surfaces per-comment in the ratings dialog.
      cell: (r) => (r.rating ? html`<span title=${ratingLabel(r.rating)}>${r.rating}</span>` : "—") },
    { key: "type", label: t("search_type"), width: "100px", sortable: true,
      sortVal: (r) => r.type || "", cell: (r) => typeLabel(r.type) },
    { key: "status", label: t("downloads_status_label"), width: "120px", sortable: true,
      sortVal: (r) => r.status || "", cell: (r) => searchStatusBadge(r.status) },
    // Browse-only: the folder inside the peer's share, empty on every
    // server/Kad hit. Sorts folder-then-name, like the desktop.
    { key: "directory", label: t("search_directory"), width: "180px", sortable: true,
      sortVal: (r) => ((r.directory || "") + "/" + (r.name || "")).toLowerCase(),
      cell: (r) => html`<span title=${r.directory}>${r.directory || "—"}</span>` },
    { key: "length", label: t("downloads_detail_media_length"), num: true, width: "100px", sortable: true,
      sortVal: (r) => (r.media && r.media.length_s) || 0,
      cell: (r) => (r.media && r.media.length_s) ? formatDuration(r.media.length_s) : "" },
    { key: "bitrate", label: t("downloads_detail_media_bitrate"), num: true, width: "90px", sortable: true,
      sortVal: (r) => (r.media && r.media.bitrate) || 0,
      cell: (r) => (r.media && r.media.bitrate) ? formatInt(r.media.bitrate) : "" },
    { key: "codec", label: t("downloads_detail_media_codec"), width: "90px", sortable: true,
      sortVal: (r) => (r.media && r.media.codec) || "", cell: (r) => (r.media && r.media.codec) || "" },
    { key: "actions", label: t("search_actions"), cls: "row-actions", width: "220px",
      cell: (r) => html`
        <span class="admin-only">
          <select class="input input-sm" value=${catFor(r.hash)}
                  onChange=${(e) => setUi({ rowCat: { ...ui.rowCat, [r.hash]: Number(e.target.value) } })}>
            ${catOptions()}
          </select>
          <button class="btn btn-icon btn-sm" type="button" title=${t("search_download")} onClick=${() => downloadOne(r.hash)}>
            <${Icon} name="downloads" />
          </button>
        </span>
        <button class="btn btn-icon btn-sm" type="button" title=${t("search_comments")} onClick=${() => setCommentsFor(r)}>
          <${Icon} name="star" />
        </button>` },
  ];

  const list = sortRows(filtered, columns, sortKey, sortDir);
  // sortRows keeps the full column set (it looks columns up by key); only the
  // VirtualTable is fed the visible subset.
  const shown = columns.filter((c) => !c.key || !hidden.has(c.key));
  const rowClass = (r) => {
    const c = [];
    if (ui.selection.has(r.hash)) c.push("row-selected");
    if (r.already_have) c.push("row-have");
    return c.join(" ");
  };

  const running = tab.state === "running";
  // Toolbar split like the Shared Files one: selection actions left of the
  // rule, search actions right of it. The rule is admin-only so it goes away
  // with the group it separates instead of leading a guest's toolbar.
  // Extend greys out rather than hiding, like Stop, and its title names which
  // of the three conditions is in the way -- the last being the daemon having
  // told us this search can no longer be widened (409 kad_more_exhausted).
  const canExtend = tab.kind === "kad" && running && !tab.moreExhausted;
  const extendTitle = tab.kind !== "kad" ? t("search_extend_kad_only")
    : !running ? t("search_extend_finished")
    : tab.moreExhausted ? t("search_extend_exhausted")
    : t("search_extend_title");
  return html`
    <div class="toolbar pane-toolbar">
      <select class="input input-sm admin-only" value=${Number(ui.cat) || 0}
              onChange=${(e) => setUi({ cat: Number(e.target.value) })}>
        ${catOptions()}
      </select>
      <button class="btn btn-sm admin-only" type="button" onClick=${downloadSelected}>${t("search_download")}</button>
      <button class="btn btn-sm admin-only" type="button" onClick=${relatedSearch}
              title=${t("search_related_title")}>${t("search_related")}</button>
      <span class="selected-count admin-only">${t("search_selected")} ${selectedCount}</span>
      <span class="vsep admin-only" aria-hidden="true"></span>
      <button class="btn btn-sm admin-only" type="button" disabled=${!running}
              onClick=${() => searches.stop(tab.id)}>${t("search_stop")}</button>
      <button class="btn btn-sm admin-only" type="button" disabled=${!canExtend}
              title=${extendTitle} onClick=${() => searches.more(tab.id)}>${t("search_extend")}</button>
      <button class="btn btn-sm" type="button" onClick=${() => searches.refresh(tab.id)}>${t("search_update_results")}</button>
      <span class="search-progress">${running ? t("search_searching_fmt", { percent: tab.percent || 0 }) : ""}</span>
      <div class="spacer"></div>
      <select class="input input-sm" value=${ui.filterHave} onChange=${(e) => setFilterHave(e.target.value)}>
        <option value="all">${t("downloads_status_all")}</option>
        <option value="not_have">${t("search_have_no")}</option>
        <option value="have">${t("search_have_yes")}</option>
      </select>
      <input class="input input-sm" type="text" placeholder=${t("search_filter")} value=${ui.filter} onInput=${(e) => setFilter(e.target.value)} />
      <${ColumnPicker} columns=${columns} hidden=${hidden} onToggle=${toggleCol} onReset=${resetPrefs} />
    </div>

    <${VirtualTable} columns=${shown} rows=${list} rowKey=${(r) => r.hash} rowClass=${rowClass}
                     sortKey=${sortKey} sortDir=${sortDir} onSort=${toggleSort}
                     widths=${widths} onResize=${setWidth}
                     maxHeight="none"
                     empty=${listPlaceholder(running, t("search_empty"))} />
    ${commentsFor ? html`<${ResultComments} result=${commentsFor} onClose=${() => setCommentsFor(null)} />` : null}`;
}

// Kad ratings/comments for one search hit. POLLING is the only way to see the
// notes land: comments_updated fires for downloads only, never for a search
// hit. amuleapi refreshes the owning search on read, so this also works on a
// finished search -- which is when a user actually reads a result list.
function ResultComments({ result, onClose }) {
  const [d, setD] = useState(null);
  const [nonce, setNonce] = useState(0);

  useEffect(() => {
    let alive = true, timer = 0, tries = 0;
    const load = () => api.get("search/results/" + result.hash + "/comments")
      .then((x) => {
        if (!alive) return;
        setD(x);
        // Capped: the Kad lookup lives ~45 s, so a stuck flag must not poll
        // for ever.
        if (x.kad_comment_search_running && tries++ < 30) timer = setTimeout(load, 2000);
      })
      .catch(() => { if (alive) setD({ count: 0, comments: [] }); });
    load();
    return () => { alive = false; clearTimeout(timer); };
  }, [result.hash, nonce]);

  const running = !!(d && d.kad_comment_search_running);
  const list = (d && d.comments) || [];
  const getKad = () => api.post("search/results/" + result.hash + "/comments")
    .then(() => { toast(t("comments_kad_started"), "info"); setNonce((n) => n + 1); })
    .catch((e) => toast(e.code === "amuled_rejected" ? t("comments_kad_error") : terr(e), "error"));

  return html`
    <div class="modal-overlay" onClick=${(e) => { if (e.target === e.currentTarget) onClose(); }}>
      <div class="modal modal-wide">
        <div class="modal-header">
          <h3 title=${result.name}>${result.name}</h3>
        </div>
        <div class="comments-head">
          <span class="comments-count">${tn("comments_count", list.length)}</span>
          <button class="btn btn-sm admin-only" type="button" onClick=${getKad} disabled=${running}>
            <${Icon} name=${running ? "polling" : "kad"} />
            ${running ? t("comments_kad_searching") : t("comments_get_kad")}
          </button>
        </div>
        <${CommentsList} comments=${list} />
        <div class="modal-actions">
          <button class="btn" type="button" onClick=${onClose}>${t("common_close")}</button>
        </div>
      </div>
    </div>`;
}

function field(label, control, cls = "") {
  return html`<div class=${"field " + cls}><label>${label}</label>${control}</div>`;
}

// Lowercase file-type token ("videos"/"audio"/…) -> capitalized label, "—" when
// the hit has no extension. ponytail: token capitalizado; añadir i18n por valor
// si se pide traducción por tipo.
function typeLabel(type) {
  return type ? type.replace(/^./, (c) => c.toUpperCase()) : "—";
}

// Search-result download status -> badge, mirroring downloads.js statusBadge.
// "new" (fresh remote hit) shows nothing to keep the common case clean.
function searchStatusBadge(s) {
  if (!s || s === "new") return null;
  const cls = s === "downloaded" ? "downloading" : s === "queued" ? "waiting"
    : (s === "canceled" || s === "queued_canceled") ? "stopped" : "";
  return html`<${Badge} kind=${cls}>${t("search_status_" + s)}<//>`;
}
