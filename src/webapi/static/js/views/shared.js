// Shared files view: list published files with session/total transfer,
// request and accept counters; change upload priority (per-row or bulk),
// multi-select with select-all, status and text filters, live totals, reload shares,
// bulk Verify Local Data over the selection.
// Live via the SSE "shared" channel.

import { api, bulkFailures } from "../api.js";
import { data } from "../events.js";
import { html, useState, useEffect, useStore } from "../dom.js";
import { checkCell, listPlaceholder, toast, confirmDialog } from "../components.js";
import { VirtualTable, sortRows, textMatcher, useTablePrefs, ColumnPicker } from "../table.js";
import { formatBytes, formatFreeSpace, formatInt, formatSpeed, formatTimestamp, twin } from "../format.js";
import { t, tn, terr } from "../i18n.js";
import { SharedDetail } from "./shared-detail.js";
import { SplitDetail } from "./split-detail.js";

const PRIORITIES = ["auto", "very_low", "low", "normal", "high", "release"]
  .map((v) => [v, t("shared_prio_" + v)]);

const STATUS_FILTERS = [["all", t("shared_status_all")], ["uploading", t("shared_status_uploading")]];

export default function Shared({ isGuest }) {
  // undefined until the first snapshot lands, [] once the share is known
  // empty; listPlaceholder tells the two apart.
  const rawShared = useStore("shared");
  const shared = rawShared || [];
  const loading = rawShared === undefined;
  const disk = (useStore("status") || {}).disk || {};
  const [selection, setSelection] = useState(() => new Set());
  const { sortKey, sortDir, hidden, widths, toggleSort, toggleCol, setWidth, resetPrefs } =
    useTablePrefs("shared", { sortKey: "name", sortDir: 1, hidden: ["last_upload", "shared_since"] });
  const [filterStatus, setFilterStatus] = useState("all");
  const [filterText, setFilterText] = useState("");
  const [verifying, setVerifying] = useState(false);
  const [detailHash, setDetailHash] = useState(null); // row shown in the detail panel

  // Open (or toggle closed) the detail panel; ignore clicks on a row's own
  // controls (the select-all/select checkbox and the priority dropdown).
  const onRowClick = (s, e) => {
    if (e.target.closest("input,select,button,a,label")) return;
    setDetailHash((h) => (h === s.hash ? null : s.hash));
  };

  useEffect(() => {
    data.register({ key: "shared", eventPrefix: "shared", id: "hash",
      list: () => api.get("shared").then((r) => r.shared || []) });
    data.ensure("shared");
  }, []);

  // Close the detail panel if its file is no longer shared (unshared/removed).
  useEffect(() => {
    if (detailHash && !shared.some((s) => s.hash === detailHash)) setDetailHash(null);
  }, [shared]);

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
  // Re-hash every selected file against its hashset.
  //
  // ponytail: N sequential POSTs -- there is no bulk verify endpoint, and each
  // call is an EC roundtrip, so they are not fired in parallel. Switch to one
  // call if the API ever grows POST /shared/verify.
  //
  // Ineligible rows cannot be filtered out up front: `incomplete` is
  // detail-only, so a still-downloading file is discovered from its own 409
  // and counts as a failure -- cheaper than a detail fetch per selected row.
  // The button is disabled for the whole run: unlike its single-request
  // siblings this loop stays in flight for seconds, and a second click would
  // queue every selected file for a re-hash twice.
  const bulkVerify = async () => {
    const hashes = Array.from(selection);
    if (!hashes.length) { toast(t("shared_toast_no_files_selected"), "warn"); return; }
    if (!(await confirmDialog(tn("shared_verify_confirm_selected", hashes.length)))) return;
    setVerifying(true);
    const failed = [];
    try {
      for (const h of hashes) {
        try { await api.post("shared/" + h + "/verify"); }
        catch (e) { failed.push(e); }
      }
    } finally { setVerifying(false); }
    if (failed.length)
      toast(t("common_bulk_partial", { failed: failed.length, total: hashes.length,
              message: terr(failed[0]) }), "warn");
    else toast(tn("shared_verify_started_selected", hashes.length), "info");
  };
  const reload = async () => {
    try { await api.post("shared_reload"); toast(t("shared_toast_reloading"), "success"); setTimeout(() => data.refresh("shared"), 1500); }
    catch (e) { toast(terr(e) || t("shared_error"), "error"); }
  };

  // --- derived ----------------------------------------------------------
  let list = shared.slice();
  // `uploading` is a count of peers being served, so "uploading" means > 0.
  if (filterStatus === "uploading") list = list.filter((s) => s.uploading > 0);
  if (filterText) { const match = textMatcher(filterText); list = list.filter((s) => match(s.name)); }
  const allSelected = list.length > 0 && list.every((s) => selection.has(s.hash));
  const selectedCount = list.filter((s) => selection.has(s.hash)).length;

  // Drop selected hashes hidden by the current filter (keep the still-visible).
  useEffect(() => {
    setSelection((prev) => {
      const vis = new Set(list.map((s) => s.hash));
      const next = new Set(); prev.forEach((h) => vis.has(h) && next.add(h));
      return next.size === prev.size ? prev : next;
    });
  }, [filterStatus, filterText]);

  const columns = [
    { always: true, cls: "check", width: "40px",
      label: checkCell(allSelected, toggleAll, t("shared_select_all")),
      cell: (s) => checkCell(selection.has(s.hash), (v) => toggleRow(s.hash, v)) },
    { key: "name", always: true, label: t("shared_name"), cls: "name", sortable: true,
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
    { key: "upspeed", label: t("shared_upload_speed"), num: true, width: "100px", sortable: true,
      sortVal: (s) => s.upload_speed_bps || 0, cell: (s) => formatSpeed(s.upload_speed_bps) },
    { key: "uploading", label: t("shared_upload_clients"), num: true, width: "90px", sortable: true,
      sortVal: (s) => s.uploading || 0, cell: (s) => formatInt(s.uploading) },
    { key: "last_upload", label: t("shared_last_upload"), width: "160px", sortable: true,
      sortVal: (s) => s.last_upload || 0, cell: (s) => formatTimestamp(s.last_upload) },
    { key: "shared_since", label: t("shared_shared_since"), width: "160px", sortable: true,
      sortVal: (s) => s.shared_since || 0, cell: (s) => formatTimestamp(s.shared_since) },
    { key: "priority", label: t("shared_priority"), width: "160px", sortable: true,
      sortVal: (s) => s.priority || "", cell: (s) => isGuest
        ? prioLabel(s)
        : html`
            <select class="input input-sm admin-only" value=${prioValue(s)}
                    onChange=${(e) => setPriority(s.hash, e.target.value)}>
              ${PRIORITIES.map(([v, l]) => html`<option value=${v}>${v === "auto" && s.priority_auto ? prioLabel(s) : l}</option>`)}
            </select>` },
  ];

  list = sortRows(list, columns, sortKey, sortDir);
  const shown = columns.filter((c) => !c.key || !hidden.has(c.key));
  const rowClass = (s) =>
    (selection.has(s.hash) ? "row-selected " : "") + (s.hash === detailHash ? "row-active" : "");

  let size = 0, xs = 0, xt = 0, up = 0;
  for (const s of list) {
    size += s.size || 0;
    xs += (s.xfer && s.xfer.session) || 0;
    xt += (s.xfer && s.xfer.total) || 0;
    up += s.upload_speed_bps || 0;
  }

  const freeSpace = formatFreeSpace(disk.temp_free_bytes, disk.incoming_free_bytes);

  return html`
    <div class="split-view">
    <${SplitDetail} storageKey="shared_detail_height" open=${!!detailHash}
                    onClose=${() => setDetailHash(null)}
                    top=${html`
    <section class="card">
      <div class="view-header">
        <div class="toolbar admin-only">
          <select class="input input-sm" value=""
                  onChange=${(e) => { const v = e.target.value; e.target.value = ""; if (v) bulkPriority(v); }}>
            <option value="">${t("shared_priority")}…</option>
            ${PRIORITIES.map(([v, l]) => html`<option value=${v}>${l}</option>`)}
          </select>
          <button class="btn btn-sm" onClick=${bulkVerify} disabled=${verifying}
                  title=${t("shared_verify_tip")}>${t("shared_verify")}</button>
          <span class="selected-count">${t("shared_selected")} ${selectedCount}</span>
          <span class="vsep" aria-hidden="true"></span>
          <button class="btn btn-sm" onClick=${reload}>${t("shared_refresh_shares")}</button>
        </div>
        <div class="spacer"></div>
        <div class="toolbar">
          <select class="input input-sm" title=${t("shared_status_label")} value=${filterStatus} onChange=${(e) => setFilterStatus(e.target.value)}>
            ${STATUS_FILTERS.map(([v, l]) => html`<option value=${v}>${l}</option>`)}
          </select>
          <input class="input input-sm" type="text" placeholder=${t("shared_filter")} value=${filterText} onInput=${(e) => setFilterText(e.target.value)} />
          <${ColumnPicker} columns=${columns} hidden=${hidden} onToggle=${toggleCol} onReset=${resetPrefs} />
        </div>
      </div>
        <${VirtualTable} columns=${shown} rows=${list} rowKey=${(s) => s.hash} rowClass=${rowClass}
                         sortKey=${sortKey} sortDir=${sortDir} onSort=${toggleSort} onRowClick=${onRowClick}
                         widths=${widths} onResize=${setWidth}
                         maxHeight="none"
                         empty=${listPlaceholder(loading, t("shared_empty"))} />
        ${loading ? null : html`
        <div class="totals-line">
          <span>${tn("shared_files_count", list.length)}</span>${" · "}<span>${t("shared_size")} ${formatBytes(size)}</span>${" · "}<span>${t("shared_transferred")} ${formatBytes(xs) + " / " + formatBytes(xt)}</span>${" · "}<span>${t("shared_upload_speed")} ${formatSpeed(up)}</span>${freeSpace ? html`${" · "}<span>${t("common_free_space")} ${freeSpace}</span>` : ""}
        </div>`}
    </section>`}>
        <${SharedDetail} hash=${detailHash} />
      <//>
    </div>`;
}

function prioValue(s) { return s.priority_auto ? "auto" : s.priority; }
function prioLabel(s) {
  const found = PRIORITIES.find(([v]) => v === s.priority);
  const base = found ? found[1] : s.priority;
  return s.priority_auto ? t("shared_prio_auto") + " (" + base + ")" : base;
}
