// Downloads view: downloads queue (with actions).
// Mirrors the aMule desktop "Downloads" page: category_index tabs, status filter,
// multi-select, column sorting, per-row pause/resume/priority/category_index/cancel,
// bulk actions, clear-completed, live totals, and an ed2k adder for mobile.

import { api, bulkFailures } from "../api.js";
import { data } from "../events.js";
import { html, useState, useEffect, useStore } from "../dom.js";
import { ProgressBar, Badge, checkCell, listPlaceholder, Tabs, toast, confirmDialog, PRIORITIES, prioValue, prioLabel } from "../components.js";
import { VirtualTable, sortRows, textMatcher, useTablePrefs, ColumnPicker } from "../table.js";
import { formatBytes, formatFreeSpace, formatSpeed } from "../format.js";
import { Icon } from "../icons.js";
import { t, tn, terr } from "../i18n.js";
import { CategoriesPanel, categoryName, categoryOptions } from "./categories.js";
import { DownloadDetail } from "./download-detail.js";
import { SplitDetail } from "./split-detail.js";

const STATUS_FILTERS = ["all", "downloading", "waiting", "paused", "stopped", "completed"]
  .map((v) => [v, t("downloads_status_" + v)]);

export default function Downloads({ isGuest }) {
  // undefined until the first snapshot lands, [] once the queue is known
  // empty; listPlaceholder tells the two apart.
  const rawDownloads = useStore("downloads");
  const downloads = rawDownloads || [];
  const loading = rawDownloads === undefined;
  const disk = (useStore("status") || {}).disk || {};
  const [categories, setCategories] = useState([]);
  const [selection, setSelection] = useState(() => new Set());
  const { sortKey, sortDir, hidden, widths, toggleSort, toggleCol, setWidth, resetPrefs } =
    useTablePrefs("downloads", { sortKey: "name", sortDir: 1, hidden: ["done", "category_index"] });
  const [filterStatus, setFilterStatus] = useState("all");
  const [filterCategory, setFilterCategory] = useState("all");
  const [filterText, setFilterText] = useState("");
  const [manageCats, setManageCats] = useState(false);
  const [detailHash, setDetailHash] = useState(null); // row shown in the detail panel

  // Open (or toggle closed) the detail panel; ignore clicks landing on a row's
  // own controls (checkbox / priority / category_index / action buttons).
  const onRowClick = (d, e) => {
    if (e.target.closest("input,select,button,a,label")) return;
    setDetailHash((h) => (h === d.hash ? null : d.hash));
  };

  const loadCategories = () =>
    api.list("categories").then((r) => setCategories(r.categories || [])).catch(() => {});

  useEffect(() => {
    data.register({ key: "downloads", eventPrefix: "download", id: "hash",
      list: () => api.list("downloads?status=all").then((r) => r.downloads || []) });
    loadCategories();
    data.ensure("downloads");
  }, []);

  // A deleted category_index renumbers the survivors, so a stale filter tab can point
  // at a now-missing index (empty list, no active tab). Fall back to "all".
  useEffect(() => {
    if (filterCategory !== "all" &&
        !categories.some((c) => c.index !== 0 && String(c.index) === filterCategory)) {
      setFilterCategory("all");
    }
  }, [categories]);

  // Close the detail panel if its download leaves the queue (cancelled/cleared).
  useEffect(() => {
    if (detailHash && !downloads.some((d) => d.hash === detailHash)) setDetailHash(null);
  }, [downloads]);

  const toggleRow = (hash, checked) => {
    const next = new Set(selection);
    if (checked) next.add(hash); else next.delete(hash);
    setSelection(next);
  };
  const toggleAll = (checked) =>
    setSelection(checked ? new Set(list.map((d) => d.hash)) : new Set());

  // --- mutations --------------------------------------------------------
  const mutate = async (fn) => {
    try { await fn(); data.refresh("downloads"); }
    catch (e) { toast(terr(e) || t("downloads_error"), "error"); }
  };
  const pause = (h) => mutate(() => api.patch("downloads/" + h, { action: "pause" }));
  const resume = (h) => mutate(() => api.patch("downloads/" + h, { action: "resume" }));
  const setPriority = (h, p) => mutate(() => api.patch("downloads/" + h, { priority: p }));
  const setCategory = (h, c) => mutate(() => api.patch("downloads/" + h, { category_index: c }));

  const del = async (d) => {
    if (!(await confirmDialog(t("downloads_confirm_cancel_download", { name: d.name })))) return;
    const next = new Set(selection); next.delete(d.hash); setSelection(next);
    mutate(() => api.del("downloads/" + d.hash));
  };

  // Run one collection-level bulk mutation and report per-item failures (207).
  const runBulk = async (fn, { clearSelection = false } = {}) => {
    const hashes = Array.from(selection);
    if (!hashes.length) { toast(t("downloads_toast_no_files_selected"), "warn"); return; }
    try {
      const failed = bulkFailures(await fn(hashes));
      if (failed.length)
        toast(t("common_bulk_partial", { failed: failed.length, total: hashes.length,
                message: terr(failed[0].error) }), "warn");
      else toast(t("downloads_toast_done"), "success");
      if (clearSelection) setSelection(new Set());
    } catch (e) { toast(terr(e) || t("downloads_error"), "error"); }
    data.refresh("downloads");
  };

  const bulk = async (action) => {
    if (action === "delete") {
      const hashes = Array.from(selection);
      if (!hashes.length) { toast(t("downloads_toast_no_files_selected"), "warn"); return; }
      if (!(await confirmDialog(tn("downloads_confirm_cancel_selected", hashes.length)))) return;
      return runBulk((h) => api.del("downloads", { hashes: h }), { clearSelection: true });
    }
    return runBulk((h) => api.patch("downloads", { hashes: h, action }));
  };

  // Apply the same field change (priority/category_index) to every selected row.
  const bulkPatch = (patch) => runBulk((h) => api.patch("downloads", { hashes: h, ...patch }));

  // The handler clears in one EC roundtrip: every row it returns is an ok,
  // and any refusal is a whole-request error `mutate` already reports.
  const clearCompleted = async () => {
    if (!(await confirmDialog(t("downloads_confirm_clear_completed")))) return;
    mutate(() => api.post("downloads_clear_completed"));
  };
  // Same endpoint scoped to one hash, for the detail panel's Clear button.
  const clearOne = (h) => mutate(() => api.post("downloads_clear_completed", { hash: h }));

  // --- derived ----------------------------------------------------------
  let list = downloads.slice();
  if (filterStatus !== "all") list = list.filter((d) => matchStatus(d, filterStatus));
  if (filterCategory !== "all") list = list.filter((d) => d.category_index === Number(filterCategory));
  if (filterText) { const match = textMatcher(filterText); list = list.filter((d) => match(d.name)); }
  const allSelected = list.length > 0 && list.every((d) => selection.has(d.hash));
  const selectedCount = list.filter((d) => selection.has(d.hash)).length;

  // Keep the selection free of rows hidden by the current filters: on a filter
  // change, drop any selected hash no longer visible (the still-visible ones stay).
  useEffect(() => {
    setSelection((prev) => {
      const vis = new Set(list.map((d) => d.hash));
      const next = new Set(); prev.forEach((h) => vis.has(h) && next.add(h));
      return next.size === prev.size ? prev : next;
    });
  }, [filterStatus, filterCategory, filterText]);

  const columns = [
    { always: true, cls: "check", width: "40px",
      label: checkCell(allSelected, toggleAll, t("downloads_select_all")),
      cell: (d) => checkCell(selection.has(d.hash), (v) => toggleRow(d.hash, v)) },
    { key: "name", always: true, label: t("downloads_name"), cls: "name", sortable: true,
      sortVal: (d) => (d.name || "").toLowerCase(),
      cell: (d) => html`<span title=${d.name}>${d.name}</span>` },
    { key: "size", label: t("downloads_size"), num: true, width: "110px", sortable: true,
      sortVal: (d) => d.size_bytes || 0, cell: (d) => formatBytes(d.size_bytes) },
    { key: "done", label: t("downloads_col_done"), num: true, width: "110px", sortable: true,
      sortVal: (d) => d.completed_bytes || 0, cell: (d) => formatBytes(d.completed_bytes) },
    { key: "progress", label: t("downloads_progress"), width: "150px", sortable: true,
      sortVal: (d) => (d.progress && d.progress.percent) || 0,
      cell: (d) => html`<${ProgressBar} percent=${d.progress && d.progress.percent} />` },
    { key: "speed", label: t("downloads_speed"), num: true, width: "100px", sortable: true,
      sortVal: (d) => d.speed_bytes_per_second || 0, cell: (d) => formatSpeed(d.speed_bytes_per_second) },
    { key: "sources", label: t("downloads_sources"), num: true, width: "100px", sortable: true,
      sortVal: (d) => (d.sources && d.sources.total) || 0,
      cell: (d) => { const src = d.sources || {}; return html`<span title=${t("downloads_title_transferring_total")}>${(src.transferring || 0) + " / " + (src.total || 0)}</span>`; } },
    { key: "status", label: t("downloads_status_label"), width: "120px", sortable: true,
      sortVal: (d) => d.status || "", cell: (d) => statusBadge(d.status) },
    { key: "priority", label: t("downloads_priority"), width: "150px", sortable: true,
      sortVal: (d) => d.priority || "", cell: (d) => isGuest
        ? prioLabel(d)
        : html`
            <select class="input input-sm admin-only" value=${prioValue(d)}
                    onChange=${(e) => setPriority(d.hash, e.target.value)}>
              ${PRIORITIES.map(([v, l]) => html`<option value=${v}>${v === "auto" && d.priority_auto ? prioLabel(d) : l}</option>`)}
            </select>` },
    { key: "category_index", label: t("downloads_category"), width: "150px", sortable: true,
      sortVal: (d) => categoryName(categories, d.category_index).toLowerCase(),
      cell: (d) => isGuest
        ? categoryName(categories, d.category_index)
        : html`
            <select class="input input-sm admin-only" value=${d.category_index}
                    onChange=${(e) => setCategory(d.hash, Number(e.target.value))}>
              ${categoryOptions(categories)}
            </select>` },
    { key: "actions", label: t("downloads_actions"), cls: "row-actions admin-only", width: "90px", cell: (d) => {
        const inactive = d.status === "paused" || d.status === "stopped";
        return html`
          <button class="btn btn-icon btn-sm" title=${inactive ? t("downloads_resume") : t("downloads_pause")}
                  onClick=${() => inactive ? resume(d.hash) : pause(d.hash)}>
            <${Icon} name=${inactive ? "play" : "pause"} />
          </button>
          <button class="btn btn-icon btn-sm btn-danger" title=${t("downloads_cancel")} onClick=${() => del(d)}>
            <${Icon} name="cancel" />
          </button>`; } },
  ];

  list = sortRows(list, columns, sortKey, sortDir);
  const shown = columns.filter((c) => !c.key || !hidden.has(c.key));
  const rowClass = (d) =>
    (selection.has(d.hash) ? "row-selected " : "") + (d.hash === detailHash ? "row-active" : "");

  let size = 0, done = 0, speed = 0;
  for (const d of list) { size += d.size_bytes || 0; done += d.completed_bytes || 0; speed += d.speed_bytes_per_second || 0; }

  const freeSpace = formatFreeSpace(disk.temp_free_bytes, disk.incoming_free_bytes);

  const categoryTabs = [
    { key: "all", label: t("downloads_all"), badge: downloads.length },
    ...categories.filter((c) => c.index !== 0).map((c) => ({
      key: String(c.index), label: c.name || ("#" + c.index),
      badge: downloads.filter((d) => d.category_index === c.index).length,
    })),
  ];

  // View-level actions ride the right of the category_index tab strip (no separate
  // title row — the nav already names the page). The manage-categories mode has
  // no tab strip, so they get a plain toolbar row there.
  const actions = html`
    <button class="btn btn-sm admin-only" type="button" onClick=${clearCompleted}>
      ${t("downloads_clear_completed")}
    </button>
    <button class=${"btn btn-sm admin-only" + (manageCats ? " btn-primary" : "")}
            type="button" aria-pressed=${manageCats}
            onClick=${() => { const next = !manageCats; setManageCats(next); if (!next) loadCategories(); }}>
      ${t("downloads_manage_categories")}
    </button>`;

  return html`
    <div class="split-view">
    ${manageCats
      ? html`<div class="view-header"><div class="spacer"></div>${actions}</div>
             <${CategoriesPanel} isGuest=${isGuest} />`
      : html`<${SplitDetail} storageKey="dl_detail_height" open=${!!detailHash}
                      onClose=${() => setDetailHash(null)}
                      top=${html`
      <section class="net-pane">
      <${Tabs} tabs=${categoryTabs} active=${filterCategory}
               onSelect=${(k) => setFilterCategory(k)} extra=${actions} />
      <div class="net-pane-body">
      <div class="view-header">
        <div class="toolbar admin-only">
          <button class="btn btn-sm" onClick=${() => bulk("pause")}><${Icon} name="pause" /> ${t("downloads_pause")}</button>
          <button class="btn btn-sm" onClick=${() => bulk("resume")}><${Icon} name="play" /> ${t("downloads_resume")}</button>
          <button class="btn btn-sm" onClick=${() => bulk("stop")}><${Icon} name="stop" /> ${t("downloads_stop")}</button>
          <button class="btn btn-sm btn-danger" onClick=${() => bulk("delete")}><${Icon} name="cancel" /> ${t("downloads_cancel")}</button>
          <select class="input input-sm" value=""
                  onChange=${(e) => { const v = e.target.value; e.target.value = ""; if (v) bulkPatch({ priority: v }); }}>
            <option value="">${t("downloads_priority")}…</option>
            ${PRIORITIES.map(([v, l]) => html`<option value=${v}>${l}</option>`)}
          </select>
          <select class="input input-sm" value=""
                  onChange=${(e) => { const v = e.target.value; e.target.value = ""; if (v !== "") bulkPatch({ category_index: Number(v) }); }}>
            <option value="">${t("downloads_category")}…</option>
            ${categoryOptions(categories)}
          </select>
          <span class="selected-count">${t("downloads_selected")} ${selectedCount}</span>
        </div>
        <div class="spacer"></div>
        <div class="toolbar">
          <select class="input input-sm" title=${t("downloads_status_label")} value=${filterStatus} onChange=${(e) => setFilterStatus(e.target.value)}>
            ${STATUS_FILTERS.map(([v, l]) => html`<option value=${v}>${l}</option>`)}
          </select>
          <input class="input input-sm" type="text" placeholder=${t("downloads_filter")} value=${filterText} onInput=${(e) => setFilterText(e.target.value)} />
          <${ColumnPicker} columns=${columns} hidden=${hidden} onToggle=${toggleCol} onReset=${resetPrefs} />
        </div>
      </div>

        <${VirtualTable} columns=${shown} rows=${list} rowKey=${(d) => d.hash} rowClass=${rowClass}
                         sortKey=${sortKey} sortDir=${sortDir} onSort=${toggleSort} onRowClick=${onRowClick}
                         widths=${widths} onResize=${setWidth}
                         maxHeight="none"
                         empty=${listPlaceholder(loading, t("downloads_empty"))} />
        ${loading ? null : html`
        <div class="totals-line">
          <span>${tn("downloads_files_count", list.length)}</span>${" · "}<span>${t("downloads_size")} ${formatBytes(size)}</span>${" · "}<span>${t("downloads_col_done")} ${formatBytes(done)}</span>${" · "}<span>${t("downloads_speed")} ${formatSpeed(speed)}</span>${freeSpace ? html`${" · "}<span>${t("common_free_space")} ${freeSpace}</span>` : ""}
        </div>`}
      </div>
    </section>`}>
        <${DownloadDetail} hash=${detailHash} isGuest=${isGuest} categories=${categories}
                           onPatch=${(h, patch) => mutate(() => api.patch("downloads/" + h, patch))}
                           onDelete=${del} onClear=${clearOne} />
      <//>`}
    </div>`;
}

// --- helpers ------------------------------------------------------------
function matchStatus(d, f) {
  if (f === "downloading") return d.status === "downloading";
  if (f === "waiting") return d.status === "waiting" || d.status === "hashing" || d.status === "allocating";
  if (f === "paused") return d.status === "paused";
  if (f === "stopped") return d.status === "stopped";
  if (f === "completed") return d.status === "completed" || d.status === "completing";
  return true;
}
function statusBadge(s) {
  const cls = s === "downloading" ? "downloading" : s === "paused" ? "paused"
    : s === "stopped" ? "stopped"
    : (s === "erroneous" || s === "insufficient_disk") ? "error"
    : (s === "waiting" || s === "hashing" || s === "allocating") ? "waiting" : "";
  // t() falls back to the raw status for values without a key.
  return html`<${Badge} kind=${cls}>${t("downloads_status_" + s)}<//>`;
}
