// Downloads view: downloads queue (with actions) + bottom Peers panel.
// Mirrors the aMule desktop "Downloads" page: category tabs, status filter,
// multi-select, column sorting, per-row pause/resume/priority/category/cancel,
// bulk actions, clear-completed, live totals, an ed2k adder for mobile, and a
// bottom Peers panel (see peers.js).

import { api } from "../api.js";
import { data } from "../events.js";
import { html, useState, useEffect, useStore } from "../dom.js";
import { ProgressBar, Badge, Placeholder, Tabs, toast, confirmDialog } from "../components.js";
import { formatBytes, formatSpeed } from "../format.js";
import { Icon } from "../icons.js";
import { t, tn } from "../i18n.js";
import { CategoriesPanel } from "./categories.js";
import { PeersPanel } from "./peers.js";

const PRIORITIES = ["auto", "very_low", "low", "normal", "high", "release"]
  .map((v) => [v, t("downloads_prio_" + v)]);
const STATUS_FILTERS = ["all", "downloading", "waiting", "paused", "completed"]
  .map((v) => [v, t("downloads_status_" + v)]);

export default function Downloads({ isGuest }) {
  const downloads = useStore("downloads") || [];
  const [categories, setCategories] = useState([]);
  const [selection, setSelection] = useState(() => new Set());
  const [sortKey, setSortKey] = useState("name");
  const [sortDir, setSortDir] = useState(1);
  const [filterStatus, setFilterStatus] = useState("all");
  const [filterCategory, setFilterCategory] = useState("all");
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
    setSelection(checked ? new Set(downloads.map((d) => d.hash)) : new Set());

  // --- mutations --------------------------------------------------------
  const mutate = async (fn) => {
    try { await fn(); data.refresh("downloads"); }
    catch (e) { toast(e.message || t("downloads_error"), "error"); }
  };
  const pause = (h) => mutate(() => api.patch("downloads/" + h, { status: "paused" }));
  const resume = (h) => mutate(() => api.patch("downloads/" + h, { status: "resumed" }));
  const setPriority = (h, p) => mutate(() => api.patch("downloads/" + h, { priority: p }));
  const setCategory = (h, c) => mutate(() => api.patch("downloads/" + h, { category: c }));

  const del = async (d) => {
    if (!(await confirmDialog(t("downloads_confirm_cancel_download", { name: d.name })))) return;
    const next = new Set(selection); next.delete(d.hash); setSelection(next);
    mutate(() => api.del("downloads/" + d.hash));
  };

  const bulk = async (action) => {
    const hashes = Array.from(selection);
    if (!hashes.length) { toast(t("downloads_toast_no_files_selected"), "warn"); return; }
    if (action === "delete" && !(await confirmDialog(tn("downloads_confirm_cancel_selected", hashes.length)))) return;
    const op = (h) => action === "pause" ? api.patch("downloads/" + h, { status: "paused" })
      : action === "resume" ? api.patch("downloads/" + h, { status: "resumed" })
      : api.del("downloads/" + h);
    try {
      await Promise.all(hashes.map(op));
      if (action === "delete") setSelection(new Set());
      toast(t("downloads_toast_done"), "success");
    } catch (e) { toast(e.message || t("downloads_error"), "error"); }
    data.refresh("downloads");
  };

  // Apply the same field change (priority/category) to every selected row.
  const bulkPatch = async (patch) => {
    const hashes = Array.from(selection);
    if (!hashes.length) { toast(t("downloads_toast_no_files_selected"), "warn"); return; }
    try {
      await Promise.all(hashes.map((h) => api.patch("downloads/" + h, patch)));
      toast(t("downloads_toast_done"), "success");
    } catch (e) { toast(e.message || t("downloads_error"), "error"); }
    data.refresh("downloads");
  };

  const clearCompleted = async () => {
    if (!(await confirmDialog(t("downloads_confirm_clear_completed")))) return;
    mutate(() => api.post("downloads/clear_completed"));
  };

  // --- derived ----------------------------------------------------------
  let list = downloads.slice();
  if (filterStatus !== "all") list = list.filter((d) => matchStatus(d, filterStatus));
  if (filterCategory !== "all") list = list.filter((d) => d.category === Number(filterCategory));
  list.sort((a, b) => sortDir * cmp(sortVal(a, sortKey), sortVal(b, sortKey)));

  let size = 0, done = 0, speed = 0;
  for (const d of list) { size += d.size || 0; done += d.size_done || 0; speed += d.speed_bps || 0; }

  const allSelected = downloads.length > 0 && downloads.every((d) => selection.has(d.hash));

  // --- header cells -----------------------------------------------------
  const sortArrow = (key) => key === sortKey
    ? html`<span class="sort-arrow"><${Icon} name=${sortDir > 0 ? "sort-asc" : "sort-desc"} /></span>` : null;
  const Th = (label, key, num) => html`
    <th class=${(key ? "sortable " : "") + (num ? "num" : "")} onClick=${key ? () => toggleSort(key) : null}>
      ${label}${sortArrow(key)}
    </th>`;

  const downloadRow = (d) => {
    const selected = selection.has(d.hash);
    const paused = d.status === "paused";
    const src = d.sources || {};
    return html`
      <tr class=${selected ? "row-selected" : ""}>
        <td><input type="checkbox" checked=${selected} onChange=${(e) => toggleRow(d.hash, e.target.checked)} /></td>
        <td class="name" title=${d.name}>${d.name}</td>
        <td class="num">${formatBytes(d.size)}</td>
        <td class="num">${formatBytes(d.size_done)}</td>
        <td><${ProgressBar} percent=${d.progress && d.progress.percent} /></td>
        <td class="num">${formatSpeed(d.speed_bps)}</td>
        <td class="num" title=${t("downloads_title_transferring_total")}>${(src.transferring || 0) + " / " + (src.total || 0)}</td>
        <td>${statusBadge(d.status)}</td>
        ${isGuest
          ? html`<td>${prioLabel(d)}</td>`
          : html`<td>
              <select class="input input-sm admin-only" value=${prioValue(d)}
                      onChange=${(e) => setPriority(d.hash, e.target.value)}>
                ${PRIORITIES.map(([v, l]) => html`<option value=${v}>${v === "auto" && d.priority_auto ? prioLabel(d) : l}</option>`)}
              </select></td>`}
        ${isGuest
          ? html`<td>${categoryName(d.category)}</td>`
          : html`<td>
              <select class="input input-sm admin-only" value=${d.category}
                      onChange=${(e) => setCategory(d.hash, Number(e.target.value))}>
                <option value=${0}>${t("downloads_category_none")}</option>
                ${categories.filter((c) => c.index !== 0).map((c) => html`<option value=${c.index}>${c.name || ("#" + c.index)}</option>`)}
              </select></td>`}
        <td class="row-actions admin-only">
          <button class="btn btn-icon btn-sm" title=${paused ? t("downloads_resume") : t("downloads_pause")}
                  onClick=${() => paused ? resume(d.hash) : pause(d.hash)}>
            <${Icon} name=${paused ? "play" : "pause"} />
          </button>
          <button class="btn btn-icon btn-sm btn-danger" title=${t("downloads_cancel")} onClick=${() => del(d)}>
            <${Icon} name="cancel" />
          </button>
        </td>
      </tr>`;
  };

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
        </div>
        <div class="spacer"></div>
        <div class="toolbar">
          <label>${t("downloads_status_label")}:</label>
          <select class="input input-sm" value=${filterStatus} onChange=${(e) => setFilterStatus(e.target.value)}>
            ${STATUS_FILTERS.map(([v, l]) => html`<option value=${v}>${l}</option>`)}
          </select>
        </div>
      </div>

      <div class="table-wrap">
        <table class="data">
          <thead>
            <tr>
              <th><input type="checkbox" title=${t("downloads_select_all")} checked=${allSelected}
                         onChange=${(e) => toggleAll(e.target.checked)} /></th>
              ${Th(t("downloads_name"), "name")}${Th(t("downloads_size"), "size", true)}${Th(t("downloads_col_done"), "done", true)}
              ${Th(t("downloads_progress"))}${Th(t("downloads_speed"), "speed", true)}${Th(t("downloads_sources"), "sources", true)}
              ${Th(t("downloads_status_label"), "status")}${Th(t("downloads_priority"), "priority")}${Th(t("downloads_category"))}
              <th class="admin-only">${t("downloads_actions")}</th>
            </tr>
          </thead>
          <tbody>
            ${list.length
              ? list.map(downloadRow)
              : html`<tr><td colspan="11"><${Placeholder} kind="info">${t("downloads_empty")}<//></td></tr>`}
          </tbody>
        </table>
      </div>

      <div class="totals-line">
        <span>${tn("downloads_files_count", list.length)}</span> · <span>${t("downloads_size")} ${formatBytes(size)}</span> ·
        <span>${t("downloads_col_done")} ${formatBytes(done)}</span> · <span>${t("downloads_speed")} ${formatSpeed(speed)}</span>
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
    : (s === "waiting" || s === "hashing" || s === "allocating") ? "waiting" : "";
  // t() falls back to the raw status for values without a key.
  return html`<${Badge} kind=${cls}>${t("downloads_status_" + s)}<//>`;
}
function sortVal(d, k) {
  switch (k) {
    case "name": return (d.name || "").toLowerCase();
    case "size": return d.size || 0;
    case "done": return d.size_done || 0;
    case "speed": return d.speed_bps || 0;
    case "sources": return (d.sources && d.sources.total) || 0;
    case "status": return d.status || "";
    case "priority": return d.priority || "";
    default: return 0;
  }
}
function cmp(a, b) { return a < b ? -1 : a > b ? 1 : 0; }
