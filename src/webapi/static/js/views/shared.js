// Shared files view: list published files with session/total transfer,
// request and accept counters; change upload priority (per-row or bulk),
// multi-select with select-all, text filter, live totals, reload shares.
// Live via the SSE "shared" channel.

import { api, bulkFailures } from "../api.js";
import { data } from "../events.js";
import { html, useState, useEffect, useStore } from "../dom.js";
import { Placeholder, toast } from "../components.js";
import { VirtualTable, sortRows, textMatcher } from "../table.js";
import { formatBytes, formatInt } from "../format.js";
import { t, tn, terr } from "../i18n.js";

const PRIORITIES = ["auto", "very_low", "low", "normal", "high", "release"]
  .map((v) => [v, t("shared_prio_" + v)]);

export default function Shared({ isGuest }) {
  const shared = useStore("shared") || [];
  const [selection, setSelection] = useState(() => new Set());
  const [sortKey, setSortKey] = useState("name");
  const [sortDir, setSortDir] = useState(1);
  const [filterText, setFilterText] = useState("");

  useEffect(() => {
    data.register({ key: "shared", eventPrefix: "shared", id: "hash",
      list: () => api.get("shared").then((r) => r.shared || []) });
    data.ensure("shared");
  }, []);

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
    setSelection(checked ? new Set(list.map((s) => s.hash)) : new Set());

  const setPriority = async (hash, p) => {
    try { await api.patch("shared/" + hash, { priority: p }); data.refresh("shared"); }
    catch (e) { toast(terr(e) || t("shared_error"), "error"); }
  };
  // Apply one priority to every selected row via the collection bulk endpoint.
  const bulkPriority = async (p) => {
    const hashes = Array.from(selection);
    if (!hashes.length) { toast(t("shared_toast_no_files_selected"), "warn"); return; }
    try {
      const failed = bulkFailures(await api.patch("shared", { hashes, priority: p }));
      if (failed.length)
        toast(t("common_bulk_partial", { failed: failed.length, total: hashes.length,
                message: terr(failed[0].error) }), "warn");
      else toast(t("shared_toast_done"), "success");
    } catch (e) { toast(terr(e) || t("shared_error"), "error"); }
    data.refresh("shared");
  };
  const reload = async () => {
    try { await api.post("shared/reload"); toast(t("shared_toast_reloading"), "success"); setTimeout(() => data.refresh("shared"), 1500); }
    catch (e) { toast(terr(e) || t("shared_error"), "error"); }
  };

  // --- derived ----------------------------------------------------------
  const match = textMatcher(filterText);
  let list = filterText ? shared.filter((s) => match(s.name)) : shared.slice();
  const allSelected = list.length > 0 && list.every((s) => selection.has(s.hash));
  const selectedCount = list.filter((s) => selection.has(s.hash)).length;

  // Drop selected hashes hidden by the current filter (keep the still-visible).
  useEffect(() => {
    setSelection((prev) => {
      const vis = new Set(list.map((s) => s.hash));
      const next = new Set(); prev.forEach((h) => vis.has(h) && next.add(h));
      return next.size === prev.size ? prev : next;
    });
  }, [filterText]);

  const columns = [
    { label: html`<input type="checkbox" title=${t("shared_select_all")} checked=${allSelected}
                         onChange=${(e) => toggleAll(e.target.checked)} />`, width: "40px",
      cell: (s) => html`<input type="checkbox" checked=${selection.has(s.hash)} onChange=${(e) => toggleRow(s.hash, e.target.checked)} />` },
    { key: "name", label: t("shared_name"), cls: "name", sortable: true,
      sortVal: (s) => (s.name || "").toLowerCase(),
      cell: (s) => html`<span title=${s.name}>${s.name}</span>` },
    { key: "size", label: t("shared_size"), num: true, width: "110px", sortable: true,
      sortVal: (s) => s.size || 0, cell: (s) => formatBytes(s.size) },
    { key: "xfer", label: t("shared_transferred"), num: true, width: "170px", sortable: true,
      sortVal: (s) => (s.xfer && s.xfer.total) || 0,
      cell: (s) => twin(s.xfer, "session", "total", formatBytes) },
    { key: "requests", label: t("shared_requested"), num: true, width: "120px", sortable: true,
      sortVal: (s) => (s.requests && s.requests.total) || 0,
      cell: (s) => twin(s.requests, "session", "total", formatInt) },
    { key: "accepts", label: t("shared_accepted"), num: true, width: "120px", sortable: true,
      sortVal: (s) => (s.accepts && s.accepts.total) || 0,
      cell: (s) => twin(s.accepts, "session", "total", formatInt) },
    { key: "sources", label: t("shared_complete_src"), num: true, width: "90px", sortable: true,
      sortVal: (s) => s.complete_sources || 0, cell: (s) => formatInt(s.complete_sources) },
    { label: t("shared_priority"), width: "160px", cell: (s) => isGuest
        ? prioLabel(s)
        : html`
            <select class="input input-sm admin-only" value=${prioValue(s)}
                    onChange=${(e) => setPriority(s.hash, e.target.value)}>
              ${PRIORITIES.map(([v, l]) => html`<option value=${v}>${v === "auto" && s.priority_auto ? prioLabel(s) : l}</option>`)}
            </select>` },
  ];

  list = sortRows(list, columns, sortKey, sortDir);
  const rowClass = (s) => selection.has(s.hash) ? "row-selected" : "";

  let size = 0, xs = 0, xt = 0;
  for (const s of list) {
    size += s.size || 0;
    xs += (s.xfer && s.xfer.session) || 0;
    xt += (s.xfer && s.xfer.total) || 0;
  }

  return html`
    <div class="view-header">
      <h3 class="section-title">${t("shared_title")}</h3>
      <div class="spacer"></div>
      <button class="btn btn-sm admin-only" onClick=${reload}>${t("shared_refresh_shares")}</button>
    </div>

    <section class="card">
      <div class="view-header">
        <div class="toolbar admin-only">
          <select class="input input-sm" value=""
                  onChange=${(e) => { const v = e.target.value; e.target.value = ""; if (v) bulkPriority(v); }}>
            <option value="">${t("shared_priority")}…</option>
            ${PRIORITIES.map(([v, l]) => html`<option value=${v}>${l}</option>`)}
          </select>
          <span class="selected-count">${t("shared_selected")} ${selectedCount}</span>
        </div>
        <div class="spacer"></div>
        <div class="toolbar">
          <span>${t("shared_filter")}:</span>
          <input class="input input-sm" type="text" value=${filterText} onInput=${(e) => setFilterText(e.target.value)} />
        </div>
      </div>

      <${VirtualTable} columns=${columns} rows=${list} rowKey=${(s) => s.hash} rowClass=${rowClass}
                       sortKey=${sortKey} sortDir=${sortDir} onSort=${toggleSort}
                       empty=${html`<${Placeholder} kind="info">${t("shared_empty")}<//>`} />

      <div class="totals-line">
        <span>${tn("shared_files_count", list.length)}</span>${" · "}<span>${t("shared_size")} ${formatBytes(size)}</span>${" · "}<span>${t("shared_transferred")} ${formatBytes(xs) + " / " + formatBytes(xt)}</span>
      </div>
    </section>`;
}

function twin(o, a, b, fmt) { return fmt((o && o[a]) || 0) + " / " + fmt((o && o[b]) || 0); }
function prioValue(s) { return s.priority_auto ? "auto" : s.priority; }
function prioLabel(s) {
  const found = PRIORITIES.find(([v]) => v === s.priority);
  const base = found ? found[1] : s.priority;
  return s.priority_auto ? t("shared_prio_auto") + " (" + base + ")" : base;
}
