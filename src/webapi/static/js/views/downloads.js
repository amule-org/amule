// Downloads view: downloads queue (with actions) + bottom Peers panel.
// Mirrors the aMule desktop "Downloads" page: category tabs, status filter,
// multi-select, column sorting, per-row pause/resume/priority/category/cancel,
// bulk actions, clear-completed, live totals, an ed2k adder for mobile, and a
// bottom Peers panel (see peers.js).

import { api, bulkFailures } from "../api.js";
import { data } from "../events.js";
import { html, useState, useEffect, useStore } from "../dom.js";
import { ProgressBar, Badge, Placeholder, Tabs, toast, confirmDialog } from "../components.js";
import { VirtualTable, sortRows, textMatcher } from "../table.js";
import { formatBytes, formatSpeed } from "../format.js";
import { Icon } from "../icons.js";
import { t, tn, terr } from "../i18n.js";
import { CategoriesPanel } from "./categories.js";
import { PeersPanel } from "./peers.js";

const PRIORITIES = ["auto", "very_low", "low", "normal", "high", "release"]
  .map((v) => [v, t("downloads_prio_" + v)]);
const STATUS_FILTERS = ["all", "downloading", "waiting", "paused", "stopped", "completed"]
  .map((v) => [v, t("downloads_status_" + v)]);

export default function Downloads({ isGuest }) {
  const downloads = useStore("downloads") || [];
  const [categories, setCategories] = useState([]);
  const [selection, setSelection] = useState(() => new Set());
  const [sortKey, setSortKey] = useState("name");
  const [sortDir, setSortDir] = useState(1);
  const [filterStatus, setFilterStatus] = useState("all");
  const [filterCategory, setFilterCategory] = useState("all");
  const [filterText, setFilterText] = useState("");
  const [manageCats, setManageCats] = useState(false);

  const loadCategories = () =>
    api.get("categories").then((r) => setCategories(r.categories || [])).catch(() => {});

  useEffect(() => {
    data.register({ key: "downloads", eventPrefix: "download", id: "hash",
      list: () => api.get("downloads?include_completed=1").then((r) => r.downloads || []) });
    loadCategories();
    data.ensure("downloads");
  }, []);

  // A deleted category renumbers the survivors, so a stale filter tab can point
  // at a now-missing index (empty list, no active tab). Fall back to "all".
  useEffect(() => {
    if (filterCategory !== "all" &&
        !categories.some((c) => c.index !== 0 && String(c.index) === filterCategory)) {
      setFilterCategory("all");
    }
  }, [categories]);

  const categoryName = (idx) => {
    if (idx === 0) return "—"; // category 0 = no category assigned
    const c = categories.find((c) => c.index === idx);
    return c ? c.name : String(idx);
  };

  const toggleSort = (key) => {
    if (sortKey === key) setSortDir(-sortDir);
    else { setSortKey(key); setSortDir(1); }
  };
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
  const pause = (h) => mutate(() => api.patch("downloads/" + h, { status: "paused" }));
  const resume = (h) => mutate(() => api.patch("downloads/" + h, { status: "resumed" }));
  const stop = (h) => mutate(() => api.patch("downloads/" + h, { status: "stopped" }));
  const setPriority = (h, p) => mutate(() => api.patch("downloads/" + h, { priority: p }));
  const setCategory = (h, c) => mutate(() => api.patch("downloads/" + h, { category: c }));

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
    const status = action === "pause" ? "paused" : action === "stop" ? "stopped" : "resumed";
    return runBulk((h) => api.patch("downloads", { hashes: h, status }));
  };

  // Apply the same field change (priority/category) to every selected row.
  const bulkPatch = (patch) => runBulk((h) => api.patch("downloads", { hashes: h, ...patch }));

  const clearCompleted = async () => {
    if (!(await confirmDialog(t("downloads_confirm_clear_completed")))) return;
    mutate(() => api.post("downloads/clear_completed"));
  };

  // --- derived ----------------------------------------------------------
  let list = downloads.slice();
  if (filterStatus !== "all") list = list.filter((d) => matchStatus(d, filterStatus));
  if (filterCategory !== "all") list = list.filter((d) => d.category === Number(filterCategory));
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
    { label: html`<input type="checkbox" title=${t("downloads_select_all")} checked=${allSelected}
                         onChange=${(e) => toggleAll(e.target.checked)} />`, width: "40px",
      cell: (d) => html`<input type="checkbox" checked=${selection.has(d.hash)} onChange=${(e) => toggleRow(d.hash, e.target.checked)} />` },
    { key: "name", label: t("downloads_name"), cls: "name", sortable: true,
      sortVal: (d) => (d.name || "").toLowerCase(),
      cell: (d) => html`<span title=${d.name}>${d.name}</span>` },
    { key: "size", label: t("downloads_size"), num: true, width: "110px", sortable: true,
      sortVal: (d) => d.size || 0, cell: (d) => formatBytes(d.size) },
    { key: "done", label: t("downloads_col_done"), num: true, width: "110px", sortable: true,
      sortVal: (d) => d.size_done || 0, cell: (d) => formatBytes(d.size_done) },
    { key: "progress", label: t("downloads_progress"), width: "150px", sortable: true,
      sortVal: (d) => (d.progress && d.progress.percent) || 0,
      cell: (d) => html`<${ProgressBar} percent=${d.progress && d.progress.percent} />` },
    { key: "speed", label: t("downloads_speed"), num: true, width: "100px", sortable: true,
      sortVal: (d) => d.speed_bps || 0, cell: (d) => formatSpeed(d.speed_bps) },
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
    { key: "category", label: t("downloads_category"), width: "150px", sortable: true,
      sortVal: (d) => categoryName(d.category).toLowerCase(),
      cell: (d) => isGuest
        ? categoryName(d.category)
        : html`
            <select class="input input-sm admin-only" value=${d.category}
                    onChange=${(e) => setCategory(d.hash, Number(e.target.value))}>
              <option value=${0}>${t("downloads_category_none")}</option>
              ${categories.filter((c) => c.index !== 0).map((c) => html`<option value=${c.index}>${c.name || ("#" + c.index)}</option>`)}
            </select>` },
    { label: t("downloads_actions"), cls: "row-actions admin-only", width: "130px", cell: (d) => {
        const inactive = d.status === "paused" || d.status === "stopped";
        const canStop = d.status !== "stopped" && d.status !== "completed" && d.status !== "completing";
        return html`
          <button class="btn btn-icon btn-sm" title=${inactive ? t("downloads_resume") : t("downloads_pause")}
                  onClick=${() => inactive ? resume(d.hash) : pause(d.hash)}>
            <${Icon} name=${inactive ? "play" : "pause"} />
          </button>
          ${canStop ? html`
            <button class="btn btn-icon btn-sm" title=${t("downloads_stop")} onClick=${() => stop(d.hash)}>
              <${Icon} name="stop" />
            </button>` : null}
          <button class="btn btn-icon btn-sm btn-danger" title=${t("downloads_cancel")} onClick=${() => del(d)}>
            <${Icon} name="cancel" />
          </button>`; } },
  ];

  list = sortRows(list, columns, sortKey, sortDir);
  const rowClass = (d) => selection.has(d.hash) ? "row-selected" : "";

  let size = 0, done = 0, speed = 0;
  for (const d of list) { size += d.size || 0; done += d.size_done || 0; speed += d.speed_bps || 0; }

  const categoryTabs = [
    { key: "all", label: t("downloads_all"), badge: downloads.length },
    ...categories.filter((c) => c.index !== 0).map((c) => ({
      key: String(c.index), label: c.name || ("#" + c.index),
      badge: downloads.filter((d) => d.category === c.index).length,
    })),
  ];

  return html`
    <div class="view-header">
      <h3 class="section-title">${t("downloads_download")}</h3>
      <div class="spacer"></div>
      <button class="btn btn-sm admin-only" type="button" onClick=${clearCompleted}>
        ${t("downloads_clear_completed")}
      </button>
      <button class=${"btn btn-sm admin-only" + (manageCats ? " btn-primary" : "")}
              type="button" aria-pressed=${manageCats}
              onClick=${() => { const next = !manageCats; setManageCats(next); if (!next) loadCategories(); }}>
        ${t("downloads_manage_categories")}
      </button>
    </div>

    ${manageCats
      ? html`<${CategoriesPanel} isGuest=${isGuest} />`
      : html`<section class="net-pane">
      <${Tabs} tabs=${categoryTabs} active=${filterCategory}
               onSelect=${(k) => setFilterCategory(k)} />
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
                  onChange=${(e) => { const v = e.target.value; e.target.value = ""; if (v !== "") bulkPatch({ category: Number(v) }); }}>
            <option value="">${t("downloads_category")}…</option>
            <option value=${0}>${t("downloads_category_none")}</option>
            ${categories.filter((c) => c.index !== 0).map((c) => html`<option value=${c.index}>${c.name || ("#" + c.index)}</option>`)}
          </select>
          <span class="selected-count">${t("downloads_selected")} ${selectedCount}</span>
        </div>
        <div class="spacer"></div>
        <div class="toolbar">
          <label>${t("downloads_status_label")}:</label>
          <select class="input input-sm" value=${filterStatus} onChange=${(e) => setFilterStatus(e.target.value)}>
            ${STATUS_FILTERS.map(([v, l]) => html`<option value=${v}>${l}</option>`)}
          </select>
          <span>${t("downloads_filter")}:</span>
          <input class="input input-sm" type="text" value=${filterText} onInput=${(e) => setFilterText(e.target.value)} />
        </div>
      </div>

      <${VirtualTable} columns=${columns} rows=${list} rowKey=${(d) => d.hash} rowClass=${rowClass}
                       sortKey=${sortKey} sortDir=${sortDir} onSort=${toggleSort}
                       empty=${html`<${Placeholder} kind="info">${t("downloads_empty")}<//>`} />

      <div class="totals-line">
        <span>${tn("downloads_files_count", list.length)}</span>${" · "}<span>${t("downloads_size")} ${formatBytes(size)}</span>${" · "}<span>${t("downloads_col_done")} ${formatBytes(done)}</span>${" · "}<span>${t("downloads_speed")} ${formatSpeed(speed)}</span>
      </div>
      </div>
    </section>`}

    <${PeersPanel} />`;
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
function prioValue(d) { return d.priority_auto ? "auto" : d.priority; }
function prioLabel(d) {
  const found = PRIORITIES.find(([v]) => v === d.priority);
  const base = found ? found[1] : d.priority;
  return d.priority_auto ? t("downloads_prio_auto") + " (" + base + ")" : base;
}
function statusBadge(s) {
  const cls = s === "downloading" ? "downloading" : s === "paused" ? "paused"
    : s === "stopped" ? "stopped"
    : (s === "waiting" || s === "hashing" || s === "allocating") ? "waiting" : "";
  // t() falls back to the raw status for values without a key.
  return html`<${Badge} kind=${cls}>${t("downloads_status_" + s)}<//>`;
}
